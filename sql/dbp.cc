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
#include <typeinfo>
#include <cxxabi.h>
#include "item_windowfunc.h"
#include "sql_window.h"

extern const char *dbug_print_unit(st_select_lex_unit *un); // in item.cc

static char dbug_item_small_buffer[DBUG_ITEM_BUFFER_SIZE];
void dbug_add_print_join(JOIN *join);
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


template<typename t>
bool do_print_ptr(t ptr)
{
  if (!ptr)
  {
    DBUG_CAT("nullptr");
    return false;
  }

  if (is_trash(ptr))
  {
    DBUG_CAT("trash");
    return false;
  }

  return true;
}

void dbug_strlcat(char *dest, uint dest_size, const char *src)
{
  uint left= dest_size-strlen(dest)-1;
  if (left > 0)
  {
    strncat(dest, src, left);
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
    // glibc prints a null %p as "(nil)", spell it as everywhere else
    for (char *nil= dbug_item_small_buffer;
         (nil= strstr(nil, "(nil)")) &&
         strlen(dbug_item_small_buffer) + 2 < DBUG_ITEM_BUFFER_SIZE; )
    {
      memmove(nil + 7, nil + 5, strlen(nil + 5) + 1);
      memcpy(nil, "nullptr", 7);
      nil+= 7;
    }
    dbug_strlcat(dest, dest_size, dbug_item_small_buffer);
  }
}


/*
  Stop adding list children once less than this is free, so the output
  ends with "..." rather than being cut off mid-item
*/
#define DBUG_BIG_BUFFER_RESERVE DBUG_ITEM_BUFFER_SIZE

static bool dbug_buffer_nearly_full()
{
  return strlen(dbug_big_buffer) >=
         DBUG_BIG_BUFFER_SIZE - DBUG_BIG_BUFFER_RESERVE;
}


uint indent_level;

void big_buffer_start()
{
  *dbug_big_buffer= (char)0;
  indent_level= 0;
}

void end_indent()
{
  if (indent_level > 0)
    indent_level--;
}

void print_indent()
{
  for (uint i= 0; i < indent_level; i++)
    DBUG_CAT( "  " );
}

void do_indent()
{
  print_indent();
  indent_level++;
}

void do_obj_indent()
{
  if (indent_level)
    DBUG_CAT("\n");
  do_indent();
}

void end_obj_indent()
{
  end_indent();
}

// start a new line at the current indent
static void dbug_new_line()
{
  DBUG_CAT("\n");
  print_indent();
}

/*
  start the next member of the structure being printed on a new line, at
  the current indent, which is the indent of that structure's members
*/
static void dbug_add_print_member(const char *label)
{
  DBUG_CAT(",");
  dbug_new_line();
  DBUG_CAT(label);
  DBUG_CAT(":");
}

/*
  Output conventions:
    structures   (Type*)0x1234{member:value, member:value}
    arrays/lists [element, element]
    strings      "text"           (names, aliases, SQL text)
    enums        bare word        (type:ref, ref_type:VIEW_REF)
    numbers      bare
    null pointer nullptr
  An Item is a structure whose first, unlabelled, member is its SQL text.
  A Field is printed as the member "table.field":value.
*/

// append length bytes of src, which may be dbug_item_small_buffer
static void dbug_strncat(const char *src, size_t length)
{
  size_t len= strlen(dbug_big_buffer);
  size_t left= DBUG_BIG_BUFFER_SIZE - len - 1;
  if (length > left)
    length= left;
  memcpy(dbug_big_buffer + len, src, length);
  dbug_big_buffer[len + length]= (char)0;
}


// print str in double quotes, or nullptr
static void dbug_add_print_string(const char *str, size_t length)
{
  if (!str)
  {
    DBUG_CAT("nullptr");
    return;
  }
  DBUG_CAT("\"");
  dbug_strncat(str, length);
  DBUG_CAT("\"");
}


static void dbug_add_print_string(const char *str)
{
  dbug_add_print_string(str, str ? strlen(str) : 0);
}


#define SMALL_BUF 1023
// the SQL text of an item, in double quotes
static void dbug_add_print_item_text(Item *item)
{
  if (item->type() == Item::SUBSELECT_ITEM &&
      ((Item_subselect*)item)->eliminated)
  {
    DBUG_CAT("eliminated:true");
    return;
  }
  char *buf= (char *)alloca(SMALL_BUF+1);
  String str(buf, SMALL_BUF, &my_charset_bin);
  str.length(0);
  item->print(&str, QT_VIEW_INTERNAL);
  dbug_add_print_string(str.ptr(), str.length());
}


/*
  print one element of an array of items, preceded by a separator unless it
  is the first.  Returns false (having printed "...") if the buffer is too
  full to continue.
*/
static bool dbug_add_print_element(Item *item, uint idx)
{
  if (idx)
    DBUG_CAT(",");
  if (dbug_buffer_nearly_full())
  {
    DBUG_CAT("...");
    return false;
  }
  dbug_add_print_item(item);
  return true;
}


static void dbug_add_print_item_array(const char *label, Item **items,
                                      uint count)
{
  DBUG_SPRINTF_CAT(", %s:[", label);
  for (uint i= 0; i < count; i++)
    if (!dbug_add_print_element(items[i], i))
      break;
  DBUG_CAT("]");
}


template <class T>
static void dbug_add_print_item_list(List<T> &list)
{
  List_iterator<T> li(list);
  T *sub_item;
  DBUG_CAT("[");
  for (uint i= 0; (sub_item= li++); i++)
    if (!dbug_add_print_element(sub_item, i))
      break;
  DBUG_CAT("]");
}


static void dbug_add_print_order_list(ORDER *order)
{
  DBUG_CAT("[");
  for (uint i= 0; order; order= order->next, i++)
    if (!dbug_add_print_element(*order->item, i))
      break;
  DBUG_CAT("]");
}


#if 0
// removed for now as it's taking precedence over dbp(Item)
const char *dbp(Item_args *args)
{
  *dbug_big_buffer= (char)0;
  if (args->argument_count())
  {
    DBUG_CAT("Arguments:");
    for (uint i= 0; i < args->argument_count(); i++)
    {
      Item *arg= args->arguments()[i];
      DBUG_SPRINTF_CAT("[%p: ", (void*)arg);
      dbug_add_print_item(arg);
      DBUG_CAT("]");
    }
  }
  Item *item= static_cast<Item*>(args);
  dbug_add_print_item((Item *)args);
  return dbug_big_buffer;
}
#endif


const char *dbug_print_items(List<Item> &list)
{
  big_buffer_start();
  DBUG_SPRINTF_CAT("(List<Item>*)%p", (void*) &list);
  indent_level= 1;
  dbug_add_print_item_list(list);
  return dbug_big_buffer;
}


const char *dbug_print_items_o_r(List<Item_outer_ref> &list)
{
  big_buffer_start();
  DBUG_SPRINTF_CAT("(List<Item_outer_ref>*)%p", (void*) &list);
  indent_level= 1;
  dbug_add_print_item_list(list);
  return dbug_big_buffer;
}


// demangled name of a type_info::name()
static const char *dbug_demangle(const char *mangled)
{
  static char name_buf[128];
  int status;
  char *demangled= abi::__cxa_demangle(mangled, NULL, NULL, &status);
  strmake(name_buf, status == 0 && demangled ? demangled : mangled,
          sizeof(name_buf) - 1);
  free(demangled);
  return name_buf;
}


// exact (demangled) class name of an item, using RTTI
static const char *dbug_item_class_name(Item *item)
{
  return dbug_demangle(typeid(*item).name());
}

#include "sql_test.h"
#include "sp_head.h"
#include "sp_rcontext.h"

/*
  THD::main_mem_root is private and has no accessor.  An explicit template
  instantiation may name a private member (C++ [temp.explicit]), so this
  hands out a pointer to it without changing sql_class.h.
*/
static MEM_ROOT THD::*dbug_thd_main_mem_root();
template <MEM_ROOT THD::*member> struct Dbug_thd_main_mem_root
{
  friend MEM_ROOT THD::*dbug_thd_main_mem_root() { return member; }
};
template struct Dbug_thd_main_mem_root<&THD::main_mem_root>;


static bool dbug_is_on(const MEM_ROOT *mem_root, void *ptr)
{
  return mem_root && dbug_is_mem_on_mem_root(mem_root, ptr);
}


/*
  Print a tag saying which mem_root an item was allocated on, found by
  address.  Roots that coincide get the first tag that matches.
    S:          thd->stmt_arena->mem_root: the statement, prepared statement
                or stored routine instruction
    E:          thd->mem_root: execution, freed at the end of the statement
    M:          thd->main_mem_root when it is neither of the above, e.g.
                the calling statement's items while a stored routine runs
    C:          the arena of the caller of the running stored routine
    SP:         the running stored routine's main_mem_root
    T("t1"):    TABLE::mem_root: vcol, default and check expressions
    TS("t1"):   TABLE_SHARE::mem_root
    G:          static storage, Item_true and Item_false
    ?:          none of these
*/
static void dbug_add_print_mem_root(Item *item)
{
  THD *thd= current_thd;
  if (item == Item_true || item == Item_false)
  {
    DBUG_CAT("G:");
    return;
  }
  if (!thd)
  {
    DBUG_CAT("?:");
    return;
  }
  if (thd->stmt_arena && dbug_is_on(thd->stmt_arena->mem_root, item))
    DBUG_CAT("S:");
  else if (dbug_is_on(thd->mem_root, item))
    DBUG_CAT("E:");
  else if (dbug_is_on(&(thd->*dbug_thd_main_mem_root()), item))
    DBUG_CAT("M:");
  else if (thd->spcont && thd->spcont->callers_arena &&
           dbug_is_on(thd->spcont->callers_arena->mem_root, item))
    DBUG_CAT("C:");
  /*
    sp_head inherits Query_arena privately, the C style cast reaches it
    anyway; its mem_root is &sp_head::main_mem_root
  */
  else if (thd->spcont && thd->spcont->m_sp &&
           dbug_is_on(((const Query_arena *) thd->spcont->m_sp)->mem_root,
                      item))
    DBUG_CAT("SP:");
  else
  {
    for (TABLE *table= thd->open_tables; table; table= table->next)
    {
      if (dbug_is_on(&table->mem_root, item))
      {
        DBUG_SPRINTF_CAT("T(\"%s\"):", table->alias.c_ptr());
        return;
      }
      if (table->s && dbug_is_on(&table->s->mem_root, item))
      {
        DBUG_SPRINTF_CAT("TS(\"%s\"):", table->alias.c_ptr());
        return;
      }
    }
    DBUG_CAT("?:");
  }
}


static void dbug_add_print_item(Item *item)
{
  do_obj_indent();
  if (!do_print_ptr(item))
  {
    end_obj_indent();
    return;
  }
  dbug_add_print_mem_root(item);
  DBUG_SPRINTF_CAT("(%s*)%p{", dbug_item_class_name(item), item);

  if (item->type() == Item::REF_ITEM)
  {
    /*
      Item_ref::print() dereferences ref, which may be null or
      not_found_item, so print the name instead
    */
    Item_ref *ref_item= (Item_ref *)item;
    if (ref_item->table_name.str)
      DBUG_SPRINTF_CAT("\"%s.%s\"", ref_item->table_name.str,
                       item->name.str ? item->name.str : "");
    else
      dbug_add_print_string(item->name.str);
  }
  else
    dbug_add_print_item_text(item);

  // non-null depended_from marks an outer reference
  if (Item_ident *ident= dynamic_cast<Item_ident *>(item))
  {
    if (ident->depended_from)
      DBUG_SPRINTF_CAT(", depended_from:(st_select_lex*)%p{select_number:%u}",
                       ident->depended_from,
                       ident->depended_from->select_number);
  }

  switch (item->type())
  {
  case Item::REF_ITEM:
    {
      static const char *ref_type_names[]=
        {"REF", "DIRECT_REF", "VIEW_REF", "OUTER_REF", "AGGREGATE_REF"};
      Item_ref *ref_item= (Item_ref *)item;
      DBUG_SPRINTF_CAT(", ref_type:%s, ref:",
                       ref_type_names[ref_item->ref_type()]);
      Item **ref= ref_item->ref;
      if (do_print_ptr(ref))
      {
        if (ref == not_found_item)
          DBUG_CAT("not_found_item");
        else
          dbug_add_print_item(*ref);
      }
    }
    break;
  case Item::FUNC_ITEM:
    {
      /*
        Item_name_const::type() returns the type of its value, which is
        FUNC_ITEM for NAME_CONST('x', -1), but it is not an Item_func
      */
      Item_func *func_item= dynamic_cast<Item_func *>(item);
      if (!func_item)
        break;
      if (func_item->functype() == Item_func::MULT_EQUAL_FUNC)
      {
        Item_equal *item_equal= (Item_equal *)func_item;
        DBUG_SPRINTF_CAT(", compare:\"%s\", const:",
                         item_equal->compare_type_handler()->name().ptr());
        Item *itm= item_equal->get_const();
        if (itm)
          dbug_add_print_item(itm);
        else
          DBUG_CAT("nullptr");
        DBUG_CAT(", fields:[");
        Item_equal_fields_iterator it(*item_equal);
        for (uint i= 0; (itm= it++); i++)
          if (!dbug_add_print_element(itm, i))
            break;
        DBUG_CAT("]");
      }
      if (func_item->argument_count())
        dbug_add_print_item_array("args", func_item->arguments(),
                                  func_item->argument_count());
    }
    break;
  case Item::COND_ITEM:
    DBUG_CAT(", args:");
    dbug_add_print_item_list(*((Item_cond*) item)->argument_list());
    break;
  case Item::SUM_FUNC_ITEM:
    {
      Item_sum *sum_item= (Item_sum *)item;
      dbug_add_print_item_array("args", sum_item->arguments(),
                                sum_item->argument_count());
    }
    break;
  case Item::WINDOW_FUNC_ITEM:
    {
      Item_window_func *win_item= (Item_window_func *)item;
      dbug_add_print_item_array("args", win_item->arguments(),
                                win_item->argument_count());
      if (win_item->window_spec)
      {
        Window_spec *spec= win_item->window_spec;
        DBUG_CAT(", partition:");
        dbug_add_print_order_list(spec->partition_list ?
                                    spec->partition_list->first : NULL);
        DBUG_CAT(", order:");
        dbug_add_print_order_list(spec->order_list ?
                                    spec->order_list->first : NULL);
      }
    }
    break;
  case Item::ROW_ITEM:
    // Item_splocal of a ROW variable is also a ROW_ITEM, skip its elements
    if (dynamic_cast<Item_row *>(item))
    {
      DBUG_CAT(", elements:[");
      for (uint i= 0; i < item->cols(); i++)
        if (!dbug_add_print_element(item->element_index(i), i))
          break;
      DBUG_CAT("]");
    }
    break;
  case Item::CACHE_ITEM:
    {
      Item_cache *cache= (Item_cache *)item;
      DBUG_SPRINTF_CAT(", type_handler:\"%s\"",
                       item->type_handler()->name().ptr());
      // Item_cache_row::values is only allocated once example is set
      if (cache->get_item() && item->cols() > 1)
      {
        DBUG_CAT(", elements:[");
        for (uint i= 0; i < item->cols(); i++)
          if (!dbug_add_print_element(item->element_index(i), i))
            break;
        DBUG_CAT("]");
      }
      DBUG_CAT(", example:");
      if (cache->get_item())
        dbug_add_print_item(cache->get_item());
      else
        DBUG_CAT("nullptr");
    }
    break;
  case Item::EXPR_CACHE_ITEM:
    DBUG_CAT(", orig_item:");
    dbug_add_print_item(((Item_cache_wrapper *)item)->get_orig_item());
    break;
  default:
    break;
  }
  DBUG_CAT("}");
  end_obj_indent();
}


extern const char *dbug_print_items(Item *item)
{
  big_buffer_start();
  dbug_add_print_item(item);
  return dbug_big_buffer;
}


const char *dbug_print_ref_array(Ref_ptr_array *ptr_array)
{
  big_buffer_start();
  DBUG_SPRINTF_CAT("(Ref_ptr_array*)%p{size:%zu, elements:[",
                   (void*) ptr_array, ptr_array->size());
  indent_level= 1;
  for (size_t i= 0; i < ptr_array->size(); i++)
    if (!dbug_add_print_element((*ptr_array)[i], (uint) i))
      break;
  DBUG_CAT("]}");
  return dbug_big_buffer;
}


#if MYSQL_VERSION_ID > 100400
const char *dbug_print_trace(Json_writer *x)
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
      return dbug_print_trace(current_thd->opt_trace.get_current_json());
    else
      return "Trace empty";
  }
  else
    return "No Thread";
}
#endif


static void dbug_add_print_unit(st_select_lex_unit *unit);

// (st_select_lex*)0x1234{"select ..."[, master_unit:...]}
static void dbug_add_print_select(st_select_lex *sl, bool with_unit= false)
{
  DBUG_SPRINTF_CAT("(st_select_lex*)%p", sl);
  if (!sl)
    return;

  char *buf= dbug_item_small_buffer;
  String str(buf, sizeof(dbug_item_small_buffer), &my_charset_bin);
  str.length(0);

  THD *thd= current_thd;
  ulonglong save_option_bits= thd->variables.option_bits;
  thd->variables.option_bits &= ~OPTION_QUOTE_SHOW_CREATE;

  sl->print(thd, &str, enum_query_type(QT_SHOW_SELECT_NUMBER | QT_EXPLAIN));

  thd->variables.option_bits= save_option_bits;

  DBUG_CAT("{");
  dbug_add_print_string(str.ptr(), str.length());
  if (with_unit)
  {
    indent_level++;
    dbug_add_print_member("master_unit");
    dbug_add_print_unit(sl->master_unit());
    indent_level--;
  }
  DBUG_CAT("}");
}


// (st_select_lex_unit*)0x1234{"select ... union select ..."}
static void dbug_add_print_unit(st_select_lex_unit *unit)
{
  DBUG_SPRINTF_CAT("(st_select_lex_unit*)%p", unit);
  if (!unit)
    return;
  DBUG_CAT("{");
  dbug_add_print_string(dbug_print_unit(unit));
  DBUG_CAT("}");
}


void dbug_add_print_jointab(JOIN_TAB *join_tab);

void dbug_add_print_lex(LEX *lex)
{
#if MYSQL_VERSION_ID > 100400
  st_select_lex *sl, *select_lex= lex->first_select_lex();
#else
  st_select_lex *sl, *select_lex= &lex->select_lex;
#endif

  DBUG_SPRINTF_CAT("(LEX*)%p{select_lex:", lex);
  indent_level++;
  dbug_add_print_select(select_lex);

  dbug_add_print_member("all_selects_list");
  DBUG_CAT("[");
  for (sl= lex->all_selects_list; sl; sl= sl->next_select_in_list())
  {
    if (sl != lex->all_selects_list)
      DBUG_CAT(", ");
    dbug_add_print_select(sl);
  }
  DBUG_CAT("]");

  if (select_lex->master_unit() && (select_lex->master_unit()->is_unit_op() ||
      select_lex->master_unit()->fake_select_lex))
  {
    // this is union or similar of more than one select
    dbug_add_print_member("unit_op");     // TODO...
    DBUG_CAT("true");
  }
  else
  {
    dbug_add_print_member("table_list");
    DBUG_CAT("[");
    for (TABLE_LIST *tbl= select_lex->table_list.first; tbl;
         tbl= tbl->next_global)
    {
      if (tbl != select_lex->table_list.first)
        DBUG_CAT(", ");
      DBUG_SPRINTF_CAT("(TABLE_LIST*)%p{alias:", tbl);
      dbug_add_print_string(tbl->alias.str, tbl->alias.length);
      if (tbl->select_stmt.str)
      {
        DBUG_CAT(", select_stmt:");
        dbug_add_print_string(tbl->select_stmt.str, tbl->select_stmt.length);
      }
      DBUG_CAT("}");
    }
    DBUG_CAT("]");
    dbug_add_print_member("join");
    dbug_add_print_join(select_lex->join);
  }
  DBUG_CAT("}");
  indent_level--;
}


const char *dbug_print_lex(LEX *lex)
{
  big_buffer_start();
  dbug_add_print_lex(lex);
  return dbug_big_buffer;
}


// print a field's value: NULL, numbers bare, anything else in quote
static void dbug_add_print_field_value(Field *field, char quote)
{
  if (field->is_real_null())
  {
    DBUG_CAT("NULL");
    return;
  }
  String tmp;
  bool is_number= field->type() == MYSQL_TYPE_BIT;
  if (is_number)
    (void) field->val_int_as_str(&tmp, 1);
  else
  {
    field->val_str(&tmp);
    Item_result cmp= field->cmp_type();
    is_number= cmp == INT_RESULT || cmp == REAL_RESULT ||
               cmp == DECIMAL_RESULT;
  }
  if (is_number)
    DBUG_SPRINTF_CAT("%.*s", (int) tmp.length(), tmp.ptr());
  else
    DBUG_SPRINTF_CAT("%c%.*s%c", quote, (int) tmp.length(), tmp.ptr(), quote);
}


// a value to be embedded in a double quoted string, as in "1 <= a < 'x'"
void dbug_add_print_field_value(Field *field)
{
  dbug_add_print_field_value(field, '\'');
}


/*
  "table.field":value, or just "table.field" if !show_field_values.
  The caller groups several fields in {} or [] respectively.
*/
const char *dbug_add_print_field(Field *field)
{
  DBUG_SPRINTF_CAT("\"%s.%s\"", field->table->alias.c_ptr(),
                   field->field_name.str);
  if (show_field_values)
  {
    DBUG_CAT(":");
    dbug_add_print_field_value(field, '"');
  }
  return dbug_big_buffer;
}


// all fields of a table, as {"t.a":1, "t.b":"x"} or ["t.a", "t.b"]
static void dbug_add_print_fields(Field **field)
{
  DBUG_CAT(show_field_values ? "{" : "[");
  for (Field **f= field; *f; f++)
  {
    if (f != field)
      DBUG_CAT(", ");
    dbug_add_print_field(*f);
  }
  DBUG_CAT(show_field_values ? "}" : "]");
}


static void dbug_add_print_table(TABLE *table)
{
  DBUG_SPRINTF_CAT("(TABLE*)%p", table);
  if (is_trash_or_null(table))
    return;
  DBUG_CAT("{alias:");
  dbug_add_print_string(table->alias.c_ptr());
  DBUG_CAT(", fields:");
  dbug_add_print_fields(table->field);
  DBUG_CAT("}");
}


const char *dbug_print_table(TABLE *table)
{
  big_buffer_start();
  dbug_add_print_table(table);
  return dbug_big_buffer;
}


// one TABLE_LIST, field names only
static void dbug_add_print_table_list_entry(TABLE_LIST *table_list)
{
  DBUG_SPRINTF_CAT("(TABLE_LIST*)%p{alias:", table_list);
  dbug_add_print_string(table_list->alias.str, table_list->alias.length);
  if (table_list->nested_join)
    DBUG_SPRINTF_CAT(", nested_join:(NESTED_JOIN*)%p", table_list->nested_join);
  DBUG_CAT(", table:");
  bool save_show_field_values= show_field_values;
  show_field_values= FALSE;
  dbug_add_print_table(table_list->table);
  show_field_values= save_show_field_values;
  DBUG_CAT("}");
}


// table_list and the tables following it in the next_global chain
static void dbug_add_print_table_list(TABLE_LIST *table_list)
{
  DBUG_CAT("[");
  indent_level++;
  for (TABLE_LIST *cur_table= table_list;
       cur_table != NULL;
       cur_table= cur_table->next_global)
  {
    if (cur_table != table_list)
      DBUG_CAT(",");
    dbug_new_line();
    dbug_add_print_table_list_entry(cur_table);
  }
  indent_level--;
  DBUG_CAT("]");
}


const char *dbug_print_table_list(TABLE_LIST *table_list)
{
  big_buffer_start();
  dbug_add_print_table_list(table_list);
  return dbug_big_buffer;
}


// print an item one level deeper than the current line
static void dbug_add_print_child_item(Item *item)
{
  indent_level++;
  dbug_add_print_item(item);
  indent_level--;
}


// label:N, label_name:"name" for a key number of table
static void dbug_add_print_key_no(const char *label, TABLE *table, uint key)
{
  if (key == MAX_KEY)
  {
    DBUG_SPRINTF_CAT("%s:MAX_KEY", label);
    return;
  }
  DBUG_SPRINTF_CAT("%s:%u, %s_name:", label, key, label);
  if (!is_trash_or_null(table) && key < table->s->keys)
    dbug_add_print_string(table->key_info[key].name.str,
                          table->key_info[key].name.length);
}


static void dbug_add_print_table_ref(JOIN_TAB *join_tab)
{
  TABLE_REF *ref= &join_tab->ref;
  DBUG_CAT(", ref:{");
  dbug_add_print_key_no("key", join_tab->table, (uint) ref->key);
  DBUG_SPRINTF_CAT(", key_parts:%u, key_length:%u, null_ref_key:%s, items:[",
                   ref->key_parts, ref->key_length,
                   ref->null_ref_key ? "true" : "false");
  if (!is_trash_or_null(ref->items))
  {
    indent_level++;
    for (uint i= 0; i < ref->key_parts; i++)
      if (!dbug_add_print_element(ref->items[i], i))
        break;
    indent_level--;
  }
  DBUG_CAT("]}");
}


// split a comma separated list into ["a", "b"], or [5, 5] if !quoted
static void dbug_add_print_csv_array(const String &csv, bool quoted)
{
  const char *quote= quoted ? "\"" : "";
  const char *p= csv.ptr(), *end= p + csv.length();
  DBUG_CAT("[");
  while (p < end)
  {
    const char *comma= (const char *) memchr(p, ',', end - p);
    const char *stop= comma ? comma : end;
    DBUG_SPRINTF_CAT("%s%s%.*s%s", p == csv.ptr() ? "" : ", ", quote,
                     (int) (stop - p), p, quote);
    p= stop + 1;
  }
  DBUG_CAT("]");
}


static void dbug_add_print_quick(QUICK_SELECT_I *quick)
{
  static const char *quick_type_names[]=
    {"range", "index_intersect", "index_merge", "range_desc", "fulltext",
     "ror_intersect", "ror_union", "group_min_max"};
  DBUG_SPRINTF_CAT(", quick:(QUICK_SELECT_I*)%p", quick);
  if (is_trash_or_null(quick))
    return;
  int type= quick->get_type();
  char names_buf[256], lengths_buf[256];
  String names(names_buf, sizeof(names_buf), &my_charset_bin);
  String lengths(lengths_buf, sizeof(lengths_buf), &my_charset_bin);
  names.length(0);
  lengths.length(0);
  quick->add_keys_and_lengths(&names, &lengths);
  DBUG_SPRINTF_CAT("{type:%s, keys:",
                   type >= 0 && type <= QUICK_SELECT_I::QS_TYPE_GROUP_MIN_MAX ?
                     quick_type_names[type] : "?");
  dbug_add_print_csv_array(names, true);
  DBUG_CAT(", lengths:");
  dbug_add_print_csv_array(lengths, false);
  DBUG_CAT("}");
}


// access method, conditions and execution state of a JOIN_TAB
static void dbug_add_print_jointab_access(JOIN_TAB *join_tab)
{
  static const char *sj_strategy_names[]=
    {"none", "dups_weedout", "loose_scan", "first_match", "materialize",
     "materialize_scan"};
  enum join_type type= join_tab->type;

  DBUG_SPRINTF_CAT(", type:%s",
                   type <= JT_HASH_INDEX_MERGE ? join_type_str[type] : "?");
  switch (type)
  {
  case JT_SYSTEM:
  case JT_CONST:
  case JT_EQ_REF:
  case JT_REF:
  case JT_REF_OR_NULL:
  case JT_FT:
  case JT_UNIQUE_SUBQUERY:
  case JT_INDEX_SUBQUERY:
    dbug_add_print_table_ref(join_tab);
    break;
  case JT_HASH:
    dbug_add_print_table_ref(join_tab);
    /* fall through */
  case JT_ALL:
    /*
      At execution range/index_merge access is JT_ALL or JT_HASH with
      select->quick set, see JOIN_TAB::save_explain_data()
    */
    if (join_tab->use_quick == 2)
      DBUG_CAT(", range_checked_for_each_record:true");
    if (!is_trash_or_null(join_tab->select) && join_tab->select->quick)
      dbug_add_print_quick(join_tab->select->quick);
    break;
  case JT_NEXT:
  case JT_HASH_NEXT:
    DBUG_CAT(", ");
    dbug_add_print_key_no("index", join_tab->table, join_tab->index);
    break;
  case JT_RANGE:
  case JT_INDEX_MERGE:
  case JT_HASH_RANGE:
  case JT_HASH_INDEX_MERGE:
    if (!is_trash_or_null(join_tab->select))
      dbug_add_print_quick(join_tab->select->quick);
    else
      DBUG_CAT(", select:nullptr");
    break;
  default:
    break;
  }

  dbug_add_print_member("rowid_filter");
  DBUG_SPRINTF_CAT("%p, cache_select:%p, pushed_idx_cond:%p, cache:%p",
                   join_tab->rowid_filter, join_tab->cache_select,
                   join_tab->table && join_tab->table->file ?
                     join_tab->table->file->pushed_idx_cond : NULL,
                   join_tab->cache);
  dbug_add_print_member("sj_strategy");
  DBUG_SPRINTF_CAT("%s, limit:%llu, send_records:%llu",
                   join_tab->sj_strategy <= SJ_OPT_MATERIALIZE_SCAN ?
                     sj_strategy_names[join_tab->sj_strategy] : "?",
                   (ulonglong) join_tab->limit,
                   (ulonglong) join_tab->send_records);
  dbug_add_print_member("select_cond");
  if (do_print_ptr(join_tab->select_cond))
    dbug_add_print_child_item(join_tab->select_cond);
}


// one element of JOIN_TAB::tab_list
static void dbug_add_print_tab_list_entry(TABLE_LIST *table_list)
{
  DBUG_SPRINTF_CAT("(TABLE_LIST*)%p{table_name:", table_list);
  if (table_list->table_name.str && table_list->table_name.length)
    dbug_add_print_string(table_list->table_name.str,
                          table_list->table_name.length);
  else
    DBUG_CAT("nullptr");

  DBUG_CAT(", type:");
  if (table_list->view)
  {
    if (table_list->algorithm == VIEW_ALGORITHM_MERGE)
      DBUG_CAT("merge");
    else if (table_list->algorithm == VIEW_ALGORITHM_TMPTABLE)
      DBUG_CAT("tmptable");
    else
      DBUG_CAT("view");
  }
  else if (table_list->derived)
    DBUG_CAT("derived");
  else
    DBUG_CAT("plain");

  if (!is_trash_or_null(table_list->nested_join))
    DBUG_SPRINTF_CAT(", nested_join:(NESTED_JOIN*)%p", table_list->nested_join);
  if (!is_trash_or_null(table_list->prep_on_expr))
  {
    DBUG_CAT(", prep_on_expr:");
    dbug_add_print_child_item(table_list->prep_on_expr);
  }
  if (!is_trash_or_null(table_list->jtbm_subselect))
  {
    DBUG_CAT(", jtbm_subselect:");
    dbug_add_print_child_item(table_list->jtbm_subselect);
  }
  else if (!is_trash_or_null(table_list->sj_subq_pred))
  {
    DBUG_CAT(", sj_subq_pred:");
    dbug_add_print_child_item(table_list->sj_subq_pred);
  }
  if (!is_trash_or_null(table_list->derived))
  {
    DBUG_CAT(", derived:");
    dbug_add_print_unit(table_list->derived);
  }
  DBUG_CAT("}");
}


void dbug_add_print_jointab(JOIN_TAB *join_tab)
{
  DBUG_SPRINTF_CAT("(JOIN_TAB*)%p", join_tab);
  if (is_trash_or_null(join_tab))
    return;

  if (join_tab->aggr != NULL)
  {
    DBUG_CAT("{table:\"\", type:AGGREGATE}");
    return;
  }

  // members of the JOIN_TAB are one level deeper than the JOIN_TAB itself
  indent_level++;
  DBUG_CAT("{table:");
  if (do_print_ptr(join_tab->table))
    dbug_add_print_string(join_tab->table->alias.c_ptr());

  dbug_add_print_jointab_access(join_tab);

  if (!is_trash_or_null(join_tab->tab_list))
  {
    dbug_add_print_member("tab_list");
    DBUG_CAT("[");
    indent_level++;
    for (TABLE_LIST *table_list= join_tab->tab_list; table_list;
         table_list= table_list->next_global)
    {
      if (table_list != join_tab->tab_list)
        DBUG_CAT(",");
      dbug_new_line();
      dbug_add_print_tab_list_entry(table_list);
      if (table_list == table_list->next_global)      // no loop forever
        break;
    }
    indent_level--;
    DBUG_CAT("]");
  }
  DBUG_CAT("}");
  indent_level--;
}

const char *dbug_print_jointab(JOIN_TAB *join_tab)
{
  big_buffer_start();
  dbug_add_print_jointab(join_tab);
  return dbug_big_buffer;
}


void dbug_add_print_join(JOIN *join)
{
  DBUG_SPRINTF_CAT("(JOIN*)%p", join);
  if (!join)
    return;
  // members of the JOIN are one level deeper than the JOIN itself
  indent_level++;
  DBUG_CAT("{select_lex:");
  dbug_add_print_select(join->select_lex);
  dbug_add_print_member("conds");
  dbug_add_print_child_item(join->conds);
  // elements of arrays are one level deeper than the array's label
  dbug_add_print_member("order");
  indent_level++;
  dbug_add_print_order_list(join->order);
  indent_level--;
  dbug_add_print_member("group_list");
  indent_level++;
  dbug_add_print_order_list(join->group_list);
  indent_level--;
  if (!is_trash_or_null(join->fields))
  {
    dbug_add_print_member("fields");
    indent_level++;
    dbug_add_print_item_list(*join->fields);
    indent_level--;
  }

  dbug_add_print_member("join_tab");
  DBUG_CAT("[");
  indent_level++;
  for (JOIN_TAB *tab= join->join_tab;
       tab < join->join_tab + join->exec_join_tab_cnt() + join->aggr_tables;
       tab++)
  {
    if (tab != join->join_tab)
      DBUG_CAT(",");
    dbug_new_line();
    dbug_add_print_jointab(tab);
  }
  indent_level--;
  DBUG_CAT("]");
  if (!is_trash_or_null(join->pushdown_query))
  {
    dbug_add_print_member("pushdown_query");
    dbug_add_print_select(join->pushdown_query->select_lex);
  }
  DBUG_CAT("}");
  indent_level--;
}


const char *dbug_print_join2(JOIN *join)
{
  big_buffer_start();
  dbug_add_print_join(join);
  return dbug_big_buffer;
}


const char *dbug_print_select_lex(st_select_lex *sl)
{
  big_buffer_start();
  dbug_add_print_select(sl, true);
  return dbug_big_buffer;
}


const char *dbug_print_lex_node(st_select_lex_node *node)
{
  st_select_lex *lex= (st_select_lex *)(node);

  return dbug_print_select_lex(lex);
}


const char *dbug_print_fields(Field **field)
{
  big_buffer_start();
  DBUG_SPRINTF_CAT("(Field**)%p", field);
  dbug_add_print_fields(field);
  return dbug_big_buffer;
}


const char *dbug_print_field(Field *field)
{
  big_buffer_start();
  DBUG_SPRINTF_CAT("(Field*)%p{", field);
  dbug_add_print_field(field);
  DBUG_CAT("}");
  return dbug_big_buffer;
}


#if MYSQL_VERSION_ID > 100338 && MYSQL_VERSION_ID < 100400
#include "sql_statistics.h"

const char *dbug_print_histogram(Histogram_binary *hist)
{
  *dbug_big_buffer= (char)0;
  uchar *values= hist->get_values();

  DBUG_CAT("(Histogram *)");
  if(hist->get_type() == SINGLE_PREC_HB)
  {
    DBUG_CAT("(SINGLE_PREC_HB)\n");
    for(uint i= 0; i < hist->get_width(); i++)
    {
      uint val= (uint) (((uint8 *) values)[i]);
      DBUG_SPRINTF_CAT("values[%u] = %u\n", i, val);
    }
    DBUG_CAT("\n");
  }
  if(hist->get_type() == DOUBLE_PREC_HB)
  {
    DBUG_CAT("(DOUBLE_PREC_HB)\n");
    for(uint i= 0; i < hist->get_width(); i++)
    {
      uint val= (uint) uint2korr(values + i * 2);
      DBUG_SPRINTF_CAT("values[%u] = %u\n", i, val);
    }
    DBUG_CAT("\n");
  }

  return dbug_big_buffer;
}

const char *dbug_print_col_stats(Column_statistics *stats)
{
  *dbug_big_buffer= (char)0;
  Histogram_base *hist= stats->histogram;
  uchar *values= hist->get_values();
  double min= stats->min_value->val_real();
  double max= stats->max_value->val_real();

  DBUG_CAT("(Column_statistics *)\nHistogram Data\n");
  if(hist->get_type() == SINGLE_PREC_HB)
  {
    DBUG_CAT("(SINGLE_PREC_HB)\n");
    for(uint i= 0; i < hist->get_width(); i++)
    {
      uint rval= (uint) (((uint8 *) values)[i]);
      double val= min + (max-min)*(double)rval/((uint) (1 << 8) - 1);
      DBUG_SPRINTF_CAT("values[%u] = %f\n", i, val);
    }
    DBUG_CAT("\n");
  }
  if(hist->get_type() == DOUBLE_PREC_HB)
  {
    DBUG_CAT("(DOUBLE_PREC_HB)\n");
    for(uint i= 0; i < hist->get_width(); i++)
    {
      uint rval= (uint) uint2korr(values + i * 2);
      double val= min + (max-min)*(double)rval/((uint) (1 << 16) - 1);
      DBUG_SPRINTF_CAT("values[%u] = %f\n", i, val);
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
const char *DBUG_PRINT_TRACE(Json_writer *x)   { return dbug_print_trace(x); }
#endif

const char *DBUG_PRINT_FUNCTION(table_map m)
{
  big_buffer_start();
  DBUG_SPRINTF_CAT("(table_map)0x%llx", (ulonglong) m);
  return dbug_big_buffer;
}


/*
  Catch-all for pointer types with no overload of their own.  gdb accepts a
  pointer -> integer argument as a "non-standard conversion", so without this
  dbp(unhandled *) would silently be printed by dbp(table_map).  gdb ranks
  pointer -> void* above that, and base-class pointer conversions above both.
*/
const char *DBUG_PRINT_FUNCTION(const void *p)
{
  big_buffer_start();
  DBUG_SPRINTF_CAT("(void*)%p (no dbp() overload for this type)", p);
  return dbug_big_buffer;
}


/*
  MEM_ROOT contents.  alloc_root() is a bump allocator: allocations carry no
  header, so only polymorphic objects can be found, by their vtable pointer.
  A word in a block's used area is taken as the start of an object when
    - it points into the mariadbd image,
    - vptr[-2] (offset to top) is 0, i.e. not a secondary vptr,
    - vptr[-1] is a type_info whose own vptr is one of libstdc++'s
      __class_type_info vtables.
  Polymorphic members embedded in another object are listed as well.
*/
extern "C" char __executable_start[], _end[];

struct Dbug_rtti_a { virtual ~Dbug_rtti_a() {} };
struct Dbug_rtti_b { virtual ~Dbug_rtti_b() {} };
struct Dbug_rtti_si: Dbug_rtti_a {};
struct Dbug_rtti_vmi: Dbug_rtti_a, Dbug_rtti_b {};

// [p - before, p + after) is inside the image; no pointer arithmetic on p,
// which may be any value
static bool dbug_in_image(const void *p, size_t before, size_t after)
{
  uintptr_t a= (uintptr_t) p, start= (uintptr_t) __executable_start,
            end= (uintptr_t) _end;
  return a % sizeof(void *) == 0 &&
         a >= start && a - start >= before &&
         a <= end && end - a >= after;
}


// the most derived type of the object at p, or NULL
static const std::type_info *dbug_object_type(const void *p)
{
  static const void *ti_vptr[3];
  if (!ti_vptr[0])
  {
    ti_vptr[0]= *(const void * const *) &typeid(Dbug_rtti_a);
    ti_vptr[1]= *(const void * const *) &typeid(Dbug_rtti_si);
    ti_vptr[2]= *(const void * const *) &typeid(Dbug_rtti_vmi);
  }
  const void * const *vptr= *(const void * const * const *) p;
  if (!dbug_in_image(vptr, 2 * sizeof(void *), sizeof(void *)) ||
      vptr[-2] != NULL ||
      !dbug_in_image(vptr[-1], 0, sizeof(void *)))
    return NULL;
  const void *ti_vp= *(const void * const *) vptr[-1];
  if (ti_vp != ti_vptr[0] && ti_vp != ti_vptr[1] && ti_vp != ti_vptr[2])
    return NULL;
  return (const std::type_info *) vptr[-1];
}


// objects in [start, end) of a block; items_only: skip non-Items
static void dbug_add_print_mem_objects(const char *start, const char *end,
                                       bool items_only)
{
  const abi::__class_type_info *item_ti=
    (const abi::__class_type_info *) &typeid(Item);
  DBUG_CAT(", objects:[");
  indent_level++;
  uint n= 0;
  for (const char *p= start; p + sizeof(void *) <= end; p+= sizeof(void *))
  {
    const std::type_info *ti= dbug_object_type(p);
    if (!ti)
      continue;
    /*
      upcast as catch matching does; abi::__dynamic_cast cannot, as from the
      most derived type it never looks at the bases
    */
    void *obj= (void *) p;
    Item *item= ti->__do_upcast(item_ti, &obj) ? (Item *) obj : NULL;
    if (items_only && !item)
      continue;
    if (n++)
      DBUG_CAT(",");
    dbug_new_line();
    if (dbug_buffer_nearly_full())
    {
      DBUG_CAT("...");
      break;
    }
    DBUG_SPRINTF_CAT("+0x%zx (%s*)%p", (size_t) (p - start),
                     dbug_demangle(ti->name()), p);
    if (item)
    {
      DBUG_CAT("{");
      dbug_add_print_item_text(item);
      DBUG_CAT("}");
    }
  }
  indent_level--;
  DBUG_CAT("]");
}


static void dbug_add_print_mem_blocks(const char *label, USED_MEM *block,
                                      int verbose)
{
  dbug_add_print_member(label);
  DBUG_CAT("[");
  indent_level++;
  for (uint i= 0; block; block= block->next, i++)
  {
    const char *start= (const char *) block + ALIGN_SIZE(sizeof(USED_MEM));
    const char *end= (const char *) block + block->size - block->left;
    if (i)
      DBUG_CAT(",");
    dbug_new_line();
    if (dbug_buffer_nearly_full())
    {
      DBUG_CAT("...");
      break;
    }
    DBUG_SPRINTF_CAT("(USED_MEM*)%p{size:%zu, used:%zu, left:%zu",
                     block, block->size, (size_t) (end - start), block->left);
    if (verbose)
      dbug_add_print_mem_objects(start, end, verbose == 2);
    DBUG_CAT("}");
  }
  indent_level--;
  DBUG_CAT("]");
}


/*
  verbose 0: blocks only, 1: and every polymorphic object found in them,
  2: and every Item found in them
*/
const char *DBUG_PRINT_FUNCTION(MEM_ROOT *root, int verbose)
{
  big_buffer_start();
  if (!root)
  {
    DBUG_CAT("nullptr");
    return dbug_big_buffer;
  }
  size_t total_size= 0, total_used= 0;
  for (USED_MEM *list : {root->free, root->used})
    for (USED_MEM *block= list; block; block= block->next)
    {
      total_size+= block->size;
      total_used+= block->size - block->left - ALIGN_SIZE(sizeof(USED_MEM));
    }
  DBUG_SPRINTF_CAT("(MEM_ROOT*)%p{block_num:%u, flags:0x%x, block_size:%zu, "
                   "min_malloc:%zu, total_size:%zu, total_used:%zu, "
                   "pre_alloc:%p",
                   root, root->block_num, (uint) root->flags,
                   root->block_size, root->min_malloc, total_size,
                   total_used, root->pre_alloc);
  indent_level= 1;
  dbug_add_print_mem_blocks("free", root->free, verbose);
  dbug_add_print_mem_blocks("used", root->used, verbose);
  DBUG_CAT("}");
  return dbug_big_buffer;
}

const char *DBUG_PRINT_FUNCTION(MEM_ROOT *root)
  { return DBUG_PRINT_FUNCTION(root, 0); }


// (List<T>*)0x1234["a", "b"] for any list of strings
template <class T>
static const char *dbug_print_string_list(const char *type, List<T> &x,
                                          const char *(*str)(T *),
                                          size_t (*len)(T *))
{
  big_buffer_start();
  DBUG_SPRINTF_CAT("(%s*)%p[", type, (void*) &x);
  List_iterator<T> iter(x);
  T *elem;
  for (uint i= 0; (elem= iter++); i++)
  {
    if (i)
      DBUG_CAT(", ");
    dbug_add_print_string(str(elem), len(elem));
  }
  DBUG_CAT("]");
  return dbug_big_buffer;
}


template <class T> static const char *dbug_lex_str(T *s) { return s->str; }
template <class T> static size_t dbug_lex_len(T *s) { return s->length; }


const char *DBUG_PRINT_FUNCTION(List<Lex_ident_sys> &x)
{
  return dbug_print_string_list<Lex_ident_sys>("List<Lex_ident_sys>", x,
                                               dbug_lex_str, dbug_lex_len);
}


const char *DBUG_PRINT_FUNCTION(List<LEX_STRING> &x)
{
  return dbug_print_string_list<LEX_STRING>("List<LEX_STRING>", x,
                                            dbug_lex_str, dbug_lex_len);
}


const char *DBUG_PRINT_FUNCTION(List<LEX_CSTRING> &x)
{
  return dbug_print_string_list<LEX_CSTRING>("List<LEX_CSTRING>", x,
                                             dbug_lex_str, dbug_lex_len);
}


const char *DBUG_PRINT_FUNCTION(List<const char *> x)
{
  return dbug_print_string_list<const char *>("List<const char*>", x,
    [](const char **s) -> const char * { return *s; },
    [](const char **s) -> size_t { return *s ? strlen(*s) : 0; });
}


const char *DBUG_PRINT_FUNCTION(List<String> &x)
{
  return dbug_print_string_list<String>("List<String>", x,
    [](String *s) -> const char * { return s->ptr(); },
    [](String *s) -> size_t { return s->length(); });
}


const char *DBUG_PRINT_FUNCTION(List<TABLE_LIST> &x)
{
  big_buffer_start();
  DBUG_SPRINTF_CAT("(List<TABLE_LIST>*)%p[", (void*) &x);
  List_iterator<TABLE_LIST> iter(x);
  TABLE_LIST *tbl;
  indent_level= 1;
  for (uint i= 0; (tbl= iter++); i++)
  {
    if (i)
      DBUG_CAT(",");
    dbug_new_line();
    dbug_add_print_table_list_entry(tbl);
  }
  DBUG_CAT("]");
  return dbug_big_buffer;
}


const char *DBUG_PRINT_FUNCTION(List<JOIN_TAB_RANGE> &x)
{
  big_buffer_start();
  DBUG_SPRINTF_CAT("(List<JOIN_TAB_RANGE>*)%p[", (void*) &x);
  List_iterator<JOIN_TAB_RANGE> iter(x);
  JOIN_TAB_RANGE *range;
  for (uint i= 0; (range= iter++); i++)
    DBUG_SPRINTF_CAT("%s(JOIN_TAB_RANGE*)%p{start:(JOIN_TAB*)%p,"
                     " end:(JOIN_TAB*)%p}", i ? ", " : "",
                     range, range->start, range->end);
  DBUG_CAT("]");
  return dbug_big_buffer;
}


const char *DBUG_PRINT_FUNCTION(List<Key_part_spec> &x)
{
  big_buffer_start();
  DBUG_SPRINTF_CAT("(List<Key_part_spec>*)%p[", (void*) &x);
  List_iterator<Key_part_spec> iter(x);
  Key_part_spec *part;
  for (uint i= 0; (part= iter++); i++)
  {
    DBUG_SPRINTF_CAT("%s(Key_part_spec*)%p{field_name:", i ? ", " : "", part);
    dbug_add_print_string(part->field_name.str, part->field_name.length);
    DBUG_SPRINTF_CAT(", length:%u}", part->length);
  }
  DBUG_CAT("]");
  return dbug_big_buffer;
}


const char *DBUG_PRINT_FUNCTION(List<Sql_condition> &x)
{
  static const char *level_names[]= {"note", "warning", "error"};
  big_buffer_start();
  DBUG_SPRINTF_CAT("(List<Sql_condition>*)%p[", (void*) &x);
  List_iterator<Sql_condition> iter(x);
  Sql_condition *cond;
  indent_level= 1;
  for (uint i= 0; (cond= iter++); i++)
  {
    uint level= (uint) cond->get_level();
    if (i)
      DBUG_CAT(",");
    dbug_new_line();
    DBUG_SPRINTF_CAT("(Sql_condition*)%p{level:%s, sql_errno:%u, message:",
                     cond,
                     level < array_elements(level_names) ?
                       level_names[level] : "?",
                     cond->get_sql_errno());
    dbug_add_print_string(cond->get_message_text());
    DBUG_CAT("}");
  }
  DBUG_CAT("]");
  return dbug_big_buffer;
}


const char *DBUG_PRINT_FUNCTION(ORDER *x)
{
  big_buffer_start();
  DBUG_SPRINTF_CAT("(ORDER*)%p", x);
  indent_level= 1;
  dbug_add_print_order_list(x);
  return dbug_big_buffer;
}


/*
  (TABLE*)0x1234{alias:"t1", row:{"a":1, "b":"x"}, rowid:0x0001}
  Only columns in table->read_set are printed
*/
const char* DBUG_PRINT_ROW(TABLE *table, bool print_rowid)
{
  big_buffer_start();
  DBUG_SPRINTF_CAT("(TABLE*)%p{alias:", table);
  dbug_add_print_string(table->alias.ptr(), table->alias.length());
  DBUG_CAT(", row:{");

  bool first= true;
  for (Field **pfield= table->field; *pfield ; pfield++)
  {
    Field *field=  *pfield;

    if (table->read_set &&
        !bitmap_is_set(table->read_set, field->field_index))
      continue;

    if (first)
      first= false;
    else
      DBUG_CAT(", ");

    dbug_add_print_string(field->field_name.str);
    DBUG_CAT(":");
    dbug_add_print_field_value(field, '"');
  }
  DBUG_CAT("}");
  if (print_rowid)
  {
    DBUG_CAT(", rowid:0x");
    for (uint i=0; i < table->file->ref_length; i++)
      DBUG_SPRINTF_CAT("%02x", (uchar)table->file->ref[i]);
  }
  DBUG_CAT("}");

  return dbug_big_buffer;
}


const char* DBUG_PRINT_ROW(TABLE *table)
{
  return DBUG_PRINT_ROW(table, true);
}


const char *DBUG_PRINT_FUNCTION(TABLE_LIST &tl)
{
  return dbug_print_table_list(&tl);
}


const char *DBUG_PRINT_FUNCTION(TABLE_LIST *tl)
{
  return dbug_print_table_list(tl);
}


// (KEY_PART_INFO*)0x1234{"t1.a":1}
const char *dbug_add_print_key_part(KEY_PART_INFO *kp)
{
  DBUG_SPRINTF_CAT("(KEY_PART_INFO*)%p", kp);
  if (kp)
  {
    DBUG_CAT("{");
    dbug_add_print_field(kp->field);
    DBUG_CAT("}");
  }
  return dbug_big_buffer;
}


const char *DBUG_PRINT_FUNCTION(KEY_PART_INFO *kp)
{
  big_buffer_start();
  return dbug_add_print_key_part(kp);
}


void dbug_add_print_key(KEY *k)
{
  DBUG_SPRINTF_CAT("(KEY*)%p", k);
  if (!k)
    return;
  DBUG_CAT("{name:");
  dbug_add_print_string(k->name.str, k->name.length);
  DBUG_SPRINTF_CAT(", key_length:%u, usable_key_parts:%u,"
                   " user_defined_key_parts:%u, key_part:[",
                   k->key_length, k->usable_key_parts,
                   k->user_defined_key_parts);
  for (uint i= 0; i < k->ext_key_parts; i++)
  {
    if (i)
      DBUG_CAT(", ");
    dbug_add_print_key_part(&k->key_part[i]);
  }
  DBUG_CAT("]}");
}


const char *DBUG_PRINT_FUNCTION(KEY *k)
{
  big_buffer_start();
  dbug_add_print_key(k);
  return dbug_big_buffer;
}



const char *DBUG_PRINT_FUNCTION(LEX_CSTRING *k)
{
  big_buffer_start();
  DBUG_CAT("(LEX_CSTRING)");
  dbug_add_print_string(k->str, k->length);
  return dbug_big_buffer;
}


// a select_lex's table list, linked by next_local
const char *DBUG_PRINT_FUNCTION(SQL_I_List<TABLE_LIST> *k)
{
  big_buffer_start();
  DBUG_SPRINTF_CAT("(SQL_I_List<TABLE_LIST>*)%p{elements:%u, list:[",
                   k, k->elements);
  TABLE_LIST *element= k->first;
  indent_level= 1;
  for (uint i= 0; i < k->elements && element; i++)
  {
    if (i)
      DBUG_CAT(",");
    dbug_new_line();
    dbug_add_print_table_list_entry(element);
    element= element->next_local;
  }
  DBUG_CAT("]}");
  return dbug_big_buffer;
}


const char *DBUG_PRINT_FUNCTION(KEYUSE *k)
{
  TABLE *t= k->table;

  big_buffer_start();
  DBUG_SPRINTF_CAT("(KEYUSE*)%p{table:", k);
  dbug_add_print_string(t ? t->alias.c_ptr() : NULL);
  DBUG_CAT(", key:");
  if (t && t->key_info && k->key < t->s->keys)
    dbug_add_print_key(&t->key_info[k->key]);
  else
    DBUG_SPRINTF_CAT("%u", k->key);
  DBUG_SPRINTF_CAT(", keypart:%u, val:", k->keypart);
  dbug_add_print_child_item(k->val);
  DBUG_CAT("}");
  return dbug_big_buffer;
}


const char *DBUG_PRINT_FUNCTION(KEY_FIELD *arg)
{
  big_buffer_start();
  DBUG_SPRINTF_CAT("(KEY_FIELD*)%p{", arg);
  dbug_add_print_field(arg->field);
  DBUG_CAT(", cond:");
  dbug_add_print_child_item(arg->cond);
  DBUG_CAT(", val:");
  dbug_add_print_child_item(arg->val);
  DBUG_CAT("}");
  return dbug_big_buffer;
}


// return which parts of operand are selected by this bitmap
const char *DBUG_PRINT_FUNCTION(MY_BITMAP *bitmap, void *operand)
{

  return dbug_big_buffer;
}


const char *DBUG_PRINT_FUNCTION(Name_resolution_context *ctx)
{
  big_buffer_start();
  DBUG_SPRINTF_CAT("(Name_resolution_context*)%p", ctx);

  if (do_print_ptr(ctx))
  {
    DBUG_SPRINTF_CAT("{select_lex:(st_select_lex*)%p,"
                     " outer_context:(Name_resolution_context*)%p",
                     ctx->select_lex, ctx->outer_context);

    indent_level= 1;
    dbug_add_print_member("first_name_resolution_table");
    if (ctx->first_name_resolution_table)
      dbug_add_print_table_list(ctx->first_name_resolution_table);
    else
      DBUG_CAT("nullptr");

    dbug_add_print_member("last_name_resolution_table");
    if (ctx->last_name_resolution_table)
      dbug_add_print_table_list(ctx->last_name_resolution_table);
    else
      DBUG_CAT("nullptr");
    DBUG_CAT("}");
  }

  return dbug_big_buffer;
}


void dbug_add_print_ref_to_outside(Item_subselect::Ref_to_outside *ref)
{
  DBUG_SPRINTF_CAT("(Item_subselect::Ref_to_outside*)%p", ref);
  if (do_print_ptr(ref))
  {
    DBUG_CAT("{select:");
    dbug_add_print_select(ref->select);
    DBUG_CAT(", item:");
    dbug_add_print_child_item(ref->item);
    DBUG_CAT("}");
  }
}


const char *DBUG_PRINT_FUNCTION(Item_subselect::Ref_to_outside *ref)
{
  big_buffer_start();

  dbug_add_print_ref_to_outside(ref);

  return dbug_big_buffer;
}


const char *DBUG_PRINT_FUNCTION(List<Item_subselect::Ref_to_outside> *ref)
{
  big_buffer_start();
  DBUG_SPRINTF_CAT("(List<Item_subselect::Ref_to_outside>*)%p", ref);

  if (do_print_ptr(ref))
  {
    List_iterator_fast<Item_subselect::Ref_to_outside> it(*ref);
    Item_subselect::Ref_to_outside *upper;

    DBUG_CAT("[");
    for (uint i= 0; (upper= it++); i++)
    {
      if (i)
        DBUG_CAT(",\n");
      dbug_add_print_ref_to_outside(upper);
    }
    DBUG_CAT("]");
  }

  return dbug_big_buffer;
}


const char *DBUG_PRINT_FUNCTION(Longlong *ref)
{
  big_buffer_start();
  DBUG_SPRINTF_CAT("(Longlong)%lld", ref->value());
  return dbug_big_buffer;
}
const char *DBUG_PRINT_FUNCTION(Longlong ref)
  {return DBUG_PRINT_FUNCTION(&ref);}


const char *DBUG_PRINT_FUNCTION(ULonglong *ref)
{
  big_buffer_start();
  DBUG_SPRINTF_CAT("(ULonglong)%llu", ref->value());
  return dbug_big_buffer;
}
const char *DBUG_PRINT_FUNCTION(ULonglong ref)
  {return DBUG_PRINT_FUNCTION(&ref);}


const char *DBUG_PRINT_FUNCTION(String *ref)
{
  big_buffer_start();
  DBUG_CAT("(String)");
  dbug_add_print_string(ref->ptr(), ref->length());
  return dbug_big_buffer;
}
const char *DBUG_PRINT_FUNCTION(String ref)
  {return DBUG_PRINT_FUNCTION(&ref);}


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
  { return dbug_print_join2(x); }
const char *DBUG_PRINT_FUNCTION(JOIN x)
  { return dbug_print_join2(&x); }
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
  { return dbug_print_lex_node(node); }
const char *DBUG_PRINT_FUNCTION(st_select_lex_node node)
  { return dbug_print_lex_node(&node); }
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
const char *DBUG_PRINT_FUNCTION(KEY_PART_INFO kp)
  { return DBUG_PRINT_FUNCTION(&kp); }
const char *DBUG_PRINT_FUNCTION(KEY k)
  { return DBUG_PRINT_FUNCTION(&k); }
const char *DBUG_PRINT_FUNCTION(List<Item_subselect::Ref_to_outside> &ref)
  {return DBUG_PRINT_FUNCTION(&ref);}
const char *DBUG_PRINT_FUNCTION(List<Item_field> *ref)
  {return DBUG_PRINT_FUNCTION((List<Item> *)ref);}
const char *DBUG_PRINT_FUNCTION(List<Item_field> ref)
  {return DBUG_PRINT_FUNCTION((List<Item> *)&ref);}
const char *DBUG_PRINT_FUNCTION(SQL_I_List<TABLE_LIST> &k)
  { return DBUG_PRINT_FUNCTION(&k); }
const char *DBUG_PRINT_FUNCTION(LEX_CSTRING k)
  { return DBUG_PRINT_FUNCTION(&k); }
const char *DBUG_PRINT_FUNCTION(List<Item_equal> *ref)
  {return DBUG_PRINT_FUNCTION((List<Item> *)ref);}
const char *DBUG_PRINT_FUNCTION(List<Item_equal> ref)
  {return DBUG_PRINT_FUNCTION((List<Item> *)&ref);}
const char *DBUG_PRINT_FUNCTION(int ref)
{
  big_buffer_start();
  DBUG_SPRINTF_CAT("(int)%d", ref);
  return dbug_big_buffer;
}
const char *DBUG_PRINT_FUNCTION(long ref)
{
  big_buffer_start();
  DBUG_SPRINTF_CAT("(long)%ld", ref);
  return dbug_big_buffer;
}
const char *DBUG_PRINT_FUNCTION(double ref)
{
  big_buffer_start();
  DBUG_SPRINTF_CAT("(double)%f", ref);
  return dbug_big_buffer;
}


#endif /*DBUG_OFF*/
