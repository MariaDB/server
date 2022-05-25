/*
   Copyright (c) 2000, 2018, Oracle and/or its affiliates.
   Copyright (c) 2010, 2022, MariaDB Corporation.

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


#ifdef USE_PRAGMA_IMPLEMENTATION
#pragma implementation				// gcc: Class implementation
#endif
#include "mariadb.h"                          /* NO_EMBEDDED_ACCESS_CHECKS */
#include "sql_priv.h"
#include <mysql.h>
#include <m_ctype.h>
#include "my_dir.h"
#include "sp_rcontext.h"
#include "sp_head.h"
#include "sql_trigger.h"
#include "sql_select.h"
#include "sql_show.h"                           // append_identifier
#include "sql_view.h"                           // VIEW_ANY_SQL
#include "sql_time.h"                  // str_to_datetime_with_warn,
                                       // make_truncated_value_warning
#include "sql_acl.h"                   // get_column_grant,
                                       // SELECT_ACL, UPDATE_ACL,
                                       // INSERT_ACL,
                                       // check_grant_column
#include "sql_base.h"                  // enum_resolution_type,
                                       // REPORT_EXCEPT_NOT_FOUND,
                                       // find_item_in_list,
                                       // RESOLVED_AGAINST_ALIAS, ...
#include "sql_expression_cache.h"

const String my_null_string("NULL", 4, default_charset_info);
const String my_default_string("DEFAULT", 7, default_charset_info);

/*
  item_empty_name is used when calling Item::set_name with NULL
  pointer, to make it easier to use the name in printf.
  item_used_name is used when calling Item::set_name with a 0 length
  string.
*/
const char *item_empty_name="";
const char *item_used_name= "\0";

static int save_field_in_field(Field *, bool *, Field *, bool);
Item_bool_static *Item_false;
Item_bool_static *Item_true;

/**
  Compare two Items for List<Item>::add_unique()
*/

bool cmp_items(Item *a, Item *b)
{
  return a->eq(b, FALSE);
}


/**
  Set max_sum_func_level if it is needed
*/
inline void set_max_sum_func_level(THD *thd, SELECT_LEX *select)
{
  if (thd->lex->in_sum_func &&
      thd->lex->in_sum_func->nest_level >= select->nest_level)
    set_if_bigger(thd->lex->in_sum_func->max_sum_func_level,
                  select->nest_level - 1);
}


MEM_ROOT *get_thd_memroot(THD *thd)
{
  return thd->mem_root;
}

/*****************************************************************************
** Item functions
*****************************************************************************/

/**
  Init all special items.
*/

void item_init(void)
{
  item_func_sleep_init();
  uuid_short_init();
}


void Item::raise_error_not_evaluable()
{
  Item::Print tmp(this, QT_ORDINARY);
  my_error(ER_NOT_ALLOWED_IN_THIS_CONTEXT, MYF(0), tmp.ptr());
}


void Item::push_note_converted_to_negative_complement(THD *thd)
{
  push_warning(thd, Sql_condition::WARN_LEVEL_NOTE, ER_UNKNOWN_ERROR,
               "Cast to signed converted positive out-of-range integer to "
               "it's negative complement");
}


void Item::push_note_converted_to_positive_complement(THD *thd)
{
  push_warning(thd, Sql_condition::WARN_LEVEL_NOTE, ER_UNKNOWN_ERROR,
               "Cast to unsigned converted negative integer to it's "
               "positive complement");
}


longlong Item::val_datetime_packed_result(THD *thd)
{
  MYSQL_TIME ltime, tmp;
  if (get_date_result(thd, &ltime, Datetime::Options_cmp(thd)))
    return 0;
  if (ltime.time_type != MYSQL_TIMESTAMP_TIME)
    return pack_time(&ltime);
  if ((null_value= time_to_datetime_with_warn(thd, &ltime, &tmp,
                                              TIME_CONV_NONE)))
    return 0;
  return pack_time(&tmp);
}


longlong Item::val_time_packed_result(THD *thd)
{
  MYSQL_TIME ltime;
  if (get_date_result(thd, &ltime, Time::Options_cmp(thd)))
    return 0;
  if (ltime.time_type == MYSQL_TIMESTAMP_TIME)
    return pack_time(&ltime);
  int warn= 0;
  Time tmp(&warn, &ltime, 0);
  DBUG_ASSERT(tmp.is_valid_time());
  return tmp.to_packed();
}


/*
  For the items which don't have its own fast val_str_ascii()
  implementation we provide a generic slower version,
  which converts from the Item character set to ASCII.
  For better performance conversion happens only in 
  case of a "tricky" Item character set (e.g. UCS2).
  Normally conversion does not happen.
*/
String *Item::val_str_ascii(String *str)
{
  DBUG_ASSERT(str != &str_value);
  
  uint errors;
  String *res= val_str(&str_value);
  if (!res)
    return 0;
  
  if (!(res->charset()->state & MY_CS_NONASCII))
    str= res;
  else
  {
    if ((null_value= str->copy(res->ptr(), res->length(), collation.collation,
                               &my_charset_latin1, &errors)))
      return 0;
  }

  return str;
}


String *Item::val_str_ascii_revert_empty_string_is_null(THD *thd, String *str)
{
  String *res= val_str_ascii(str);
  if (!res && (thd->variables.sql_mode & MODE_EMPTY_STRING_IS_NULL))
  {
    null_value= false;
    str->set("", 0, &my_charset_latin1);
    return str;
  }
  return res;
}


String *Item::val_str(String *str, String *converter, CHARSET_INFO *cs)
{
  String *res= val_str(str);
  if (null_value)
    return (String *) 0;

  if (!cs)
    return res;

  uint errors;
  if ((null_value= converter->copy(res->ptr(), res->length(),
                                   collation.collation, cs,  &errors)))
    return (String *) 0;

  return converter;
}


String *Item::val_string_from_real(String *str)
{
  double nr= val_real();
  if (null_value)
    return 0;					/* purecov: inspected */
  str->set_real(nr,decimals, &my_charset_numeric);
  return str;
}


String *Item::val_string_from_int(String *str)
{
  longlong nr= val_int();
  if (null_value)
    return 0;
  str->set_int(nr, unsigned_flag, &my_charset_numeric);
  return str;
}


longlong Item::val_int_from_str(int *error)
{
  char buff[MAX_FIELD_WIDTH];
  String tmp(buff,sizeof(buff), &my_charset_bin), *res;

  /*
    For a string result, we must first get the string and then convert it
    to a longlong
  */
  if (!(res= val_str(&tmp)))
  {
    *error= 0;
    return 0;
  }
  Converter_strtoll10_with_warn cnv(NULL, Warn_filter_all(),
                                    res->charset(), res->ptr(), res->length());
  *error= cnv.error();
  return cnv.result();
}


longlong Item::val_int_signed_typecast_from_str()
{
  int error;
  longlong value= val_int_from_str(&error);
  if (unlikely(!null_value && value < 0 && error == 0))
    push_note_converted_to_negative_complement(current_thd);
  return value;
}


longlong Item::val_int_unsigned_typecast_from_str()
{
  int error;
  longlong value= val_int_from_str(&error);
  if (unlikely(!null_value && error < 0))
    push_note_converted_to_positive_complement(current_thd);
  return value;
}


longlong Item::val_int_signed_typecast_from_real()
{
  double nr= val_real();
  if (null_value)
    return 0;
  Converter_double_to_longlong conv(nr, false);
  if (conv.error())
  {
    THD *thd= current_thd;
    push_warning_printf(thd, Sql_condition::WARN_LEVEL_NOTE,
                        ER_DATA_OVERFLOW, ER_THD(thd, ER_DATA_OVERFLOW),
                        ErrConvDouble(nr).ptr(), "SIGNED BIGINT");
  }
  return conv.result();
}


longlong Item::val_int_unsigned_typecast_from_real()
{
  double nr= val_real();
  if (null_value)
    return 0;
  Converter_double_to_longlong conv(nr, true);
  if (conv.error())
  {
    THD *thd= current_thd;
    push_warning_printf(thd, Sql_condition::WARN_LEVEL_NOTE,
                        ER_DATA_OVERFLOW, ER_THD(thd, ER_DATA_OVERFLOW),
                        ErrConvDouble(nr).ptr(), "UNSIGNED BIGINT");
  }
  return conv.result();
}


longlong Item::val_int_signed_typecast_from_int()
{
  longlong value= val_int();
  if (!null_value && unsigned_flag && value < 0)
    push_note_converted_to_negative_complement(current_thd);
  return value;
}


longlong Item::val_int_unsigned_typecast_from_int()
{
  longlong value= val_int();
  if (!null_value && unsigned_flag == 0 && value < 0)
    push_note_converted_to_positive_complement(current_thd);
  return value;
}


my_decimal *Item::val_decimal_from_real(my_decimal *decimal_value)
{
  double nr= val_real();
  if (null_value)
    return 0;
  double2my_decimal(E_DEC_FATAL_ERROR, nr, decimal_value);
  return (decimal_value);
}


my_decimal *Item::val_decimal_from_int(my_decimal *decimal_value)
{
  DBUG_ASSERT(fixed());
  longlong nr= val_int();
  if (null_value)
    return 0;
  int2my_decimal(E_DEC_FATAL_ERROR, nr, unsigned_flag, decimal_value);
  return decimal_value;
}


my_decimal *Item::val_decimal_from_string(my_decimal *decimal_value)
{
  String *res;

  if (!(res= val_str(&str_value)))
    return 0;

  return decimal_from_string_with_check(decimal_value, res);
}


int Item::save_time_in_field(Field *field, bool no_conversions)
{
  MYSQL_TIME ltime;
  if (get_time(field->table->in_use, &ltime))
    return set_field_to_null_with_conversions(field, no_conversions);
  field->set_notnull();
  return field->store_time_dec(&ltime, decimals);
}


int Item::save_date_in_field(Field *field, bool no_conversions)
{
  MYSQL_TIME ltime;
  THD *thd= field->table->in_use;
  if (get_date(thd, &ltime, Datetime::Options(thd)))
    return set_field_to_null_with_conversions(field, no_conversions);
  field->set_notnull();
  return field->store_time_dec(&ltime, decimals);
}


/*
  Store the string value in field directly

  SYNOPSIS
    Item::save_str_value_in_field()
    field   a pointer to field where to store
    result  the pointer to the string value to be stored

  DESCRIPTION
    The method is used by Item_*::save_in_field implementations
    when we don't need to calculate the value to store
    See Item_string::save_in_field() implementation for example

  IMPLEMENTATION
    Check if the Item is null and stores the NULL or the
    result value in the field accordingly.

  RETURN
    Nonzero value if error
*/

int Item::save_str_value_in_field(Field *field, String *result)
{
  if (null_value)
    return set_field_to_null(field);
  field->set_notnull();
  return field->store(result->ptr(), result->length(),
		      collation.collation);
}


Item::Item(THD *thd):
  name(null_clex_str), orig_name(0), is_expensive_cache(-1)
{
  DBUG_ASSERT(thd);
  base_flags= item_base_t::FIXED;
  with_flags= item_with_t::NONE;
  null_value= 0;
  marker= MARKER_UNUSED;

   /* Initially this item is not attached to any JOIN_TAB. */
  join_tab_idx= MAX_TABLES;

  /* Put item in free list so that we can free all items at end */
  next= thd->free_list;
  thd->free_list= this;
  /*
    Item constructor can be called during execution other then SQL_COM
    command => we should check thd->lex->current_select on zero (thd->lex
    can be uninitialised)
  */
  if (thd->lex->current_select)
  {
    enum_parsing_place place= 
      thd->lex->current_select->parsing_place;
    if (place == SELECT_LIST || place == IN_HAVING)
      thd->lex->current_select->select_n_having_items++;
  }
}

/*
  This is only used for static const items
*/

Item::Item():
  name(null_clex_str), orig_name(0), is_expensive_cache(-1)
{
  DBUG_ASSERT(!mysqld_server_started);          // Created early
  base_flags= item_base_t::FIXED;
  with_flags= item_with_t::NONE;
  null_value= 0;
  marker= MARKER_UNUSED;
  join_tab_idx= MAX_TABLES;
}


const TABLE_SHARE *Item::field_table_or_null()
{
  if (real_item()->type() != Item::FIELD_ITEM)
    return NULL;

  return ((Item_field *) this)->field->table->s;
}


/**
  Constructor used by Item_field, Item_ref & aggregate (sum)
  functions.

  Used for duplicating lists in processing queries with temporary
  tables.
*/
Item::Item(THD *thd, Item *item):
  Type_all_attributes(*item),
  str_value(item->str_value),
  name(item->name),
  orig_name(item->orig_name),
  base_flags(item->base_flags & ~item_base_t::FIXED),
  with_flags(item->with_flags),
  marker(item->marker),
  null_value(item->null_value),
  is_expensive_cache(-1),
  join_tab_idx(item->join_tab_idx)
{
  next= thd->free_list;				// Put in free list
  thd->free_list= this;
}


void Item::print_parenthesised(String *str, enum_query_type query_type,
                               enum precedence parent_prec)
{
  bool need_parens= precedence() < parent_prec;
  if (need_parens)
    str->append('(');
  print(str, query_type);
  if (need_parens)
    str->append(')');
}


void Item::print(String *str, enum_query_type query_type)
{
  str->append(full_name_cstring());
}


void Item::print_item_w_name(String *str, enum_query_type query_type)
{
  print(str, query_type);

  if (name.str)
  {
    DBUG_ASSERT(name.length == strlen(name.str));
    THD *thd= current_thd;
    str->append(STRING_WITH_LEN(" AS "));
    append_identifier(thd, str, &name);
  }
}


void Item::print_value(String *str)
{
  char buff[MAX_FIELD_WIDTH];
  String *ptr, tmp(buff,sizeof(buff),str->charset());
  ptr= val_str(&tmp);
  if (!ptr)
    str->append(NULL_clex_str);
  else
  {
    switch (cmp_type()) {
    case STRING_RESULT:
    case TIME_RESULT:
      append_unescaped(str, ptr->ptr(), ptr->length());
      break;
    case DECIMAL_RESULT:
    case REAL_RESULT:
    case INT_RESULT:
      str->append(*ptr);
      break;
    case ROW_RESULT:
      DBUG_ASSERT(0);
    }
  }
}


void Item::cleanup()
{
  DBUG_ENTER("Item::cleanup");
  DBUG_PRINT("enter", ("this: %p", this));
  marker= MARKER_UNUSED;
  join_tab_idx= MAX_TABLES;
  if (orig_name)
  {
    name.str=    orig_name;
    name.length= strlen(orig_name);
  }
  DBUG_VOID_RETURN;
}


/**
  cleanup() item if it is 'fixed'.

  @param arg   a dummy parameter, is not used here
*/

bool Item::cleanup_processor(void *arg)
{
  if (fixed())
    cleanup();
  return FALSE;
}


/**
  Traverse item tree possibly transforming it (replacing items).

  This function is designed to ease transformation of Item trees.
  Re-execution note: every such transformation is registered for
  rollback by THD::change_item_tree() and is rolled back at the end
  of execution by THD::rollback_item_tree_changes().

  Therefore:
  - this function can not be used at prepared statement prepare
  (in particular, in fix_fields!), as only permanent
  transformation of Item trees are allowed at prepare.
  - the transformer function shall allocate new Items in execution
  memory root (thd->mem_root) and not anywhere else: allocated
  items will be gone in the end of execution.

  If you don't need to transform an item tree, but only traverse
  it, please use Item::walk() instead.


  @param transformer    functor that performs transformation of a subtree
  @param arg            opaque argument passed to the functor

  @return
    Returns pointer to the new subtree root.  THD::change_item_tree()
    should be called for it if transformation took place, i.e. if a
    pointer to newly allocated item is returned.
*/

Item* Item::transform(THD *thd, Item_transformer transformer, uchar *arg)
{
  DBUG_ASSERT(!thd->stmt_arena->is_stmt_prepare());

  return (this->*transformer)(thd, arg);
}


/**
  Create and set up an expression cache for this item

  @param thd             Thread handle
  @param depends_on      List of the expression parameters

  @details
  The function creates an expression cache for an item and its parameters
  specified by the 'depends_on' list. Then the expression cache is placed
  into a cache wrapper that is returned as the result of the function.

  @returns
  A pointer to created wrapper item if successful, NULL - otherwise
*/

Item* Item::set_expr_cache(THD *thd)
{
  DBUG_ENTER("Item::set_expr_cache");
  Item_cache_wrapper *wrapper;
  if (likely((wrapper= new (thd->mem_root) Item_cache_wrapper(thd, this))) &&
      likely(!wrapper->fix_fields(thd, (Item**)&wrapper)))
  {
    if (likely(!wrapper->set_cache(thd)))
      DBUG_RETURN(wrapper);
  }
  DBUG_RETURN(NULL);
}


Item_ident::Item_ident(THD *thd, Name_resolution_context *context_arg,
                       const LEX_CSTRING &db_name_arg,
                       const LEX_CSTRING &table_name_arg,
                       const LEX_CSTRING &field_name_arg)
  :Item_result_field(thd), orig_db_name(db_name_arg),
   orig_table_name(table_name_arg),
   orig_field_name(field_name_arg), context(context_arg),
   db_name(db_name_arg), table_name(table_name_arg),
   field_name(field_name_arg),
   cached_table(NULL), depended_from(NULL),
   cached_field_index(NO_CACHED_FIELD_INDEX),
   can_be_depended(TRUE), alias_name_used(FALSE)
{
  name= field_name_arg;
}


Item_ident::Item_ident(THD *thd, TABLE_LIST *view_arg,
                       const LEX_CSTRING &field_name_arg)
  :Item_result_field(thd), orig_db_name(null_clex_str),
   orig_table_name(view_arg->table_name),
   orig_field_name(field_name_arg),
   /* TODO: suspicious use of first_select_lex */
   context(&view_arg->view->first_select_lex()->context),
   db_name(null_clex_str), table_name(view_arg->alias),
   field_name(field_name_arg),
   cached_table(NULL), depended_from(NULL),
   cached_field_index(NO_CACHED_FIELD_INDEX),
   can_be_depended(TRUE), alias_name_used(FALSE)
{
  name= field_name_arg;
}


/**
  Constructor used by Item_field & Item_*_ref (see Item comment)
*/

Item_ident::Item_ident(THD *thd, Item_ident *item)
  :Item_result_field(thd, item),
   orig_db_name(item->orig_db_name),
   orig_table_name(item->orig_table_name), 
   orig_field_name(item->orig_field_name),
   context(item->context),
   db_name(item->db_name),
   table_name(item->table_name),
   field_name(item->field_name),
   cached_table(item->cached_table),
   depended_from(item->depended_from),
   cached_field_index(item->cached_field_index),
   can_be_depended(item->can_be_depended),
   alias_name_used(item->alias_name_used)
{}

void Item_ident::cleanup()
{
  DBUG_ENTER("Item_ident::cleanup");
  bool was_fixed= fixed();
  Item_result_field::cleanup();
  db_name= orig_db_name; 
  table_name= orig_table_name;
  field_name= orig_field_name;
  /* Store if this Item was depended */
  if (was_fixed)
  {
    /*
      We can trust that depended_from set correctly only if this item
      was fixed
    */
    can_be_depended= MY_TEST(depended_from);
  }
  DBUG_VOID_RETURN;
}

bool Item_ident::remove_dependence_processor(void * arg)
{
  DBUG_ENTER("Item_ident::remove_dependence_processor");
  if (get_depended_from() == (st_select_lex *) arg)
    depended_from= 0;
  context= &((st_select_lex *) arg)->context;
  DBUG_RETURN(0);
}

bool Item_ident::collect_outer_ref_processor(void *param)
{
  Collect_deps_prm *prm= (Collect_deps_prm *)param;
  if (depended_from &&
      depended_from->nest_level_base == prm->nest_level_base &&
      depended_from->nest_level < prm->nest_level)
  {
    if (prm->collect)
      prm->parameters->add_unique(this, &cmp_items);
    else
      prm->count++;
  }
  return FALSE;
}


/**
  Store the pointer to this item field into a list if not already there.

  The method is used by Item::walk to collect all unique Item_field objects
  from a tree of Items into a set of items represented as a list.

  Item_cond::walk() and Item_func::walk() stop the evaluation of the
  processor function for its arguments once the processor returns
  true.Therefore in order to force this method being called for all item
  arguments in a condition the method must return false.

  @param arg  pointer to a List<Item_field>

  @return
    FALSE to force the evaluation of collect_item_field_processor
    for the subsequent items.
*/

bool Item_field::collect_item_field_processor(void *arg)
{
  DBUG_ENTER("Item_field::collect_item_field_processor");
  DBUG_PRINT("info", ("%s", field->field_name.str ?
                      field->field_name.str : "noname"));
  List<Item_field> *item_list= (List<Item_field>*) arg;
  List_iterator<Item_field> item_list_it(*item_list);
  Item_field *curr_item;
  while ((curr_item= item_list_it++))
  {
    if (curr_item->eq(this, 1))
      DBUG_RETURN(FALSE); /* Already in the set. */
  }
  item_list->push_back(this);
  DBUG_RETURN(FALSE);
}


void Item_ident::undeclared_spvar_error() const
{
  /*
    We assume this is an unknown SP variable, possibly a ROW variable.
    Print the leftmost name in the error:
      SET var=a;      -> a
      SET var=a.b;    -> a
      SET var=a.b.c;  -> a
  */
  my_error(ER_SP_UNDECLARED_VAR, MYF(0),  db_name.str    ? db_name.str :
                                          table_name.str ? table_name.str :
                                          field_name.str);
}

bool Item_field::unknown_splocal_processor(void *arg)
{
  DBUG_ENTER("Item_field::unknown_splocal_processor");
  DBUG_ASSERT(type() == FIELD_ITEM);
  undeclared_spvar_error();
  DBUG_RETURN(true);
}


bool Item_field::add_field_to_set_processor(void *arg)
{
  DBUG_ENTER("Item_field::add_field_to_set_processor");
  DBUG_PRINT("info", ("%s", field->field_name.str ? field->field_name.str :
                      "noname"));
  TABLE *table= (TABLE *) arg;
  if (field->table == table)
    bitmap_set_bit(&table->tmp_set, field->field_index);
  DBUG_RETURN(FALSE);
}


/**
   Rename fields in an expression to new field name as speficied by ALTER TABLE
*/

bool Item_field::rename_fields_processor(void *arg)
{
  Item::func_processor_rename *rename= (Item::func_processor_rename*) arg;
  List_iterator<Create_field> def_it(rename->fields);
  Create_field *def;

  while ((def=def_it++))
  {
    if (def->change.str &&
        (!db_name.str || !db_name.str[0] ||
         !my_strcasecmp(table_alias_charset, db_name.str, rename->db_name.str)) &&
        (!table_name.str || !table_name.str[0] ||
         !my_strcasecmp(table_alias_charset, table_name.str, rename->table_name.str)) &&
        !my_strcasecmp(system_charset_info, field_name.str, def->change.str))
    {
      field_name= def->field_name;
      break;
    }
  }
  return 0;
}


/**
  Check if an Item_field references some field from a list of fields.

  Check whether the Item_field represented by 'this' references any
  of the fields in the keyparts passed via 'arg'. Used with the
  method Item::walk() to test whether any keypart in a sequence of
  keyparts is referenced in an expression.

  @param arg   Field being compared, arg must be of type Field

  @retval
    TRUE  if 'this' references the field 'arg'
  @retval
    FALSE otherwise
*/

bool Item_field::find_item_in_field_list_processor(void *arg)
{
  KEY_PART_INFO *first_non_group_part= *((KEY_PART_INFO **) arg);
  KEY_PART_INFO *last_part= *(((KEY_PART_INFO **) arg) + 1);
  KEY_PART_INFO *cur_part;

  for (cur_part= first_non_group_part; cur_part != last_part; cur_part++)
  {
    if (field->eq(cur_part->field))
      return TRUE;
  }
  return FALSE;
}


/*
  Mark field in read_map

  NOTES
    This is used by filesort to register used fields in a a temporary
    column read set or to register used fields in a view or check constraint
*/

bool Item_field::register_field_in_read_map(void *arg)
{
  TABLE *table= (TABLE *) arg;
  int res= 0;
  if (table && table != field->table)
    return res;

  if (field->vcol_info &&
      !bitmap_fast_test_and_set(field->table->read_set, field->field_index))
  {
    res= field->vcol_info->expr->walk(&Item::register_field_in_read_map,1,arg);
  }
  else
    bitmap_set_bit(field->table->read_set, field->field_index);
  return res;
}

/*
  @brief
  Mark field in bitmap supplied as *arg
*/

bool Item_field::register_field_in_bitmap(void *arg)
{
  MY_BITMAP *bitmap= (MY_BITMAP *) arg;
  DBUG_ASSERT(bitmap);
  bitmap_set_bit(bitmap, field->field_index);
  return 0;
}


/*
  Mark field in write_map

  NOTES
    This is used by UPDATE to register underlying fields of used view fields.
*/

bool Item_field::register_field_in_write_map(void *arg)
{
  TABLE *table= (TABLE *) arg;
  if (field->table == table || !table)
    bitmap_set_bit(field->table->write_set, field->field_index);
  return 0;
}

/**
  Check that we are not referring to any not yet initialized fields

  Fields are initialized in this order:
  - All fields that have default value as a constant are initialized first.
  - Then user-specified values from the INSERT list
  - Then all fields that has a default expression, in field_index order.
  - Then all virtual fields, in field_index order.
  - Then auto-increment values

  This means:
  - For default fields we can't access the same field or a field after
    itself that doesn't have a non-constant default value.
  - A virtual field can't access itself or a virtual field after itself.
  - user-specified values will not see virtual fields or default expressions,
    as in INSERT t1 (a) VALUES (b);
  - no virtual fields can access auto-increment values

  This is used by fix_vcol_expr() when a table is opened

  We don't have to check fields that are marked as NO_DEFAULT_VALUE
  as the upper level will ensure that all these will be given a value.
*/

bool Item_field::check_field_expression_processor(void *arg)
{
  Field *org_field= (Field*) arg;
  if (field->flags & NO_DEFAULT_VALUE_FLAG)
    return 0;
  if ((field->default_value && field->default_value->flags) || field->vcol_info)
  {
    if (field == org_field ||
        (!org_field->vcol_info && field->vcol_info) ||
        (((field->vcol_info && org_field->vcol_info) ||
          (!field->vcol_info && !org_field->vcol_info)) &&
         field->field_index >= org_field->field_index))
    {
      my_error(ER_EXPRESSION_REFERS_TO_UNINIT_FIELD, MYF(0),
               org_field->field_name.str, field->field_name.str);
      return 1;
    }
  }
  return 0;
}

bool Item_field::update_vcol_processor(void *arg)
{
  MY_BITMAP *map= (MY_BITMAP *) arg;
  if (field->vcol_info &&
      !bitmap_fast_test_and_set(map, field->field_index))
  {
    field->vcol_info->expr->walk(&Item::update_vcol_processor, 0, arg);
    field->vcol_info->expr->save_in_field(field, 0);
  }
  return 0;
}


bool Item::check_cols(uint c)
{
  if (c != 1)
  {
    my_error(ER_OPERAND_COLUMNS, MYF(0), c);
    return 1;
  }
  return 0;
}


bool Item::check_type_or_binary(const LEX_CSTRING &opname,
                                const Type_handler *expect) const
{
  const Type_handler *handler= type_handler();
  if (handler == expect ||
      (handler->is_general_purpose_string_type() &&
       collation.collation == &my_charset_bin))
    return false;
  my_error(ER_ILLEGAL_PARAMETER_DATA_TYPE_FOR_OPERATION, MYF(0),
           handler->name().ptr(), opname.str);
  return true;
}


bool Item::check_type_general_purpose_string(const LEX_CSTRING &opname) const
{
  const Type_handler *handler= type_handler();
  if (handler->is_general_purpose_string_type())
    return false;
  my_error(ER_ILLEGAL_PARAMETER_DATA_TYPE_FOR_OPERATION, MYF(0),
           handler->name().ptr(), opname.str);
  return true;
}


bool Item::check_type_traditional_scalar(const LEX_CSTRING &opname) const
{
  const Type_handler *handler= type_handler();
  if (handler->is_traditional_scalar_type())
    return false;
  my_error(ER_ILLEGAL_PARAMETER_DATA_TYPE_FOR_OPERATION, MYF(0),
           handler->name().ptr(), opname.str);
  return true;
}


bool Item::check_type_can_return_int(const LEX_CSTRING &opname) const
{
  const Type_handler *handler= type_handler();
  if (handler->can_return_int())
    return false;
  my_error(ER_ILLEGAL_PARAMETER_DATA_TYPE_FOR_OPERATION, MYF(0),
           handler->name().ptr(), opname.str);
  return true;
}


bool Item::check_type_can_return_decimal(const LEX_CSTRING &opname) const
{
  const Type_handler *handler= type_handler();
  if (handler->can_return_decimal())
    return false;
  my_error(ER_ILLEGAL_PARAMETER_DATA_TYPE_FOR_OPERATION, MYF(0),
           handler->name().ptr(), opname.str);
  return true;
}


bool Item::check_type_can_return_real(const LEX_CSTRING &opname) const
{
  const Type_handler *handler= type_handler();
  if (handler->can_return_real())
    return false;
  my_error(ER_ILLEGAL_PARAMETER_DATA_TYPE_FOR_OPERATION, MYF(0),
           handler->name().ptr(), opname.str);
  return true;
}


bool Item::check_type_can_return_date(const LEX_CSTRING &opname) const
{
  const Type_handler *handler= type_handler();
  if (handler->can_return_date())
    return false;
  my_error(ER_ILLEGAL_PARAMETER_DATA_TYPE_FOR_OPERATION, MYF(0),
           handler->name().ptr(), opname.str);
  return true;
}


bool Item::check_type_can_return_time(const LEX_CSTRING &opname) const
{
  const Type_handler *handler= type_handler();
  if (handler->can_return_time())
    return false;
  my_error(ER_ILLEGAL_PARAMETER_DATA_TYPE_FOR_OPERATION, MYF(0),
           handler->name().ptr(), opname.str);
  return true;
}


bool Item::check_type_can_return_str(const LEX_CSTRING &opname) const
{
  const Type_handler *handler= type_handler();
  if (handler->can_return_str())
    return false;
  my_error(ER_ILLEGAL_PARAMETER_DATA_TYPE_FOR_OPERATION, MYF(0),
           handler->name().ptr(), opname.str);
  return true;
}


bool Item::check_type_can_return_text(const LEX_CSTRING &opname) const
{
  const Type_handler *handler= type_handler();
  if (handler->can_return_text())
    return false;
  my_error(ER_ILLEGAL_PARAMETER_DATA_TYPE_FOR_OPERATION, MYF(0),
           handler->name().ptr(), opname.str);
  return true;
}


bool Item::check_type_scalar(const LEX_CSTRING &opname) const
{
  /*
    fixed==true usually means than the Item has an initialized
    and reliable data type handler and attributes.
    Item_outer_ref is an exception. It copies the data type and the attributes
    from the referenced Item in the constructor, but then sets "fixed" to false,
    and re-fixes itself again in fix_inner_refs().
    This hack in Item_outer_ref should probably be refactored eventually.
    Discuss with Sanja.
  */
  DBUG_ASSERT(fixed() || type() == REF_ITEM);
  const Type_handler *handler= type_handler();
  if (handler->is_scalar_type())
    return false;
  my_error(ER_OPERAND_COLUMNS, MYF(0), 1);
  return true;
}


extern "C" {

/*
  All values greater than MY_NAME_BINARY_VALUE are
  interpreted as binary bytes.
  The exact constant value does not matter,
  but it must be greater than 0x10FFFF,
  which is the maximum possible character in Unicode.
*/
#define MY_NAME_BINARY_VALUE 0x200000

/*
  Print all binary bytes as well as zero character U+0000 in hex notation.
  Print other characters normally.
*/
static int
my_wc_mb_item_name(CHARSET_INFO *cs, my_wc_t wc, uchar *str, uchar *end)
{
  if (wc == 0 || wc >= MY_NAME_BINARY_VALUE)
  {
    if (str + 4 >= end)
      return MY_CS_TOOSMALL3;
    str[0]= '\\';
    str[1]= 'x';
    str[2]= _dig_vec_upper[(uchar) (wc >> 4)];
    str[3]= _dig_vec_upper[(uchar) wc & 0x0F];
    return 4;
  }
  return my_charset_utf8mb3_handler.wc_mb(cs, wc, str, end);
}


/*
  Scan characters and mark all illegal sequences as binary byte values,
  to have my_wc_mb_utf8_escape_name() print them using HEX notation.
*/
static int
my_mb_wc_item_name(CHARSET_INFO *cs, my_wc_t *pwc,
                   const uchar *str, const uchar *end)
{
  int rc= cs->cset->mb_wc(cs, pwc, str, end);
  if (rc == MY_CS_ILSEQ)
  {
    *pwc= MY_NAME_BINARY_VALUE + *str;
    return 1;
  }
  return rc;
}

}


static LEX_CSTRING
make_name(THD *thd,
          const char *str, size_t length, CHARSET_INFO *cs,
          size_t max_octet_length)
{
  uint errors;
  size_t dst_nbytes= length * system_charset_info->mbmaxlen;
  set_if_smaller(dst_nbytes, max_octet_length);
  char *dst= (char*) thd->alloc(dst_nbytes + 1);
  if (!dst)
    return null_clex_str;
  uint32 cnv_length= my_convert_using_func(dst, dst_nbytes, system_charset_info,
                                           my_wc_mb_item_name,
                                           str, length,
                                           cs == &my_charset_bin ?
                                             system_charset_info : cs,
                                           my_mb_wc_item_name, &errors);
  dst[cnv_length]= '\0';
  return Lex_cstring(dst, cnv_length);
}


void Item::set_name(THD *thd, const char *str, size_t length, CHARSET_INFO *cs)
{
  if (!length)
  {
    /*
      Null string are replaced by item_empty_name. This is used by AS or
      internal function like last_insert_id() to detect if we need to
      change the name later.
      Used by sql_yacc.yy in select_alias handling
    */
    name.str= str ? item_used_name : item_empty_name;
    name.length= 0;
    return;
  }

  const char *str_start= str;
  if (!cs->m_ctype || cs->mbminlen > 1)
  {
    str+= cs->scan(str, str + length, MY_SEQ_SPACES);
    length-= (uint)(str - str_start);
  }
  else
  {
    /*
      This will probably need a better implementation in the future:
      a function in CHARSET_INFO structure.
    */
    while (length && !my_isgraph(cs,*str))
    {						// Fix problem with yacc
      length--;
      str++;
    }
  }
  if (str != str_start && is_explicit_name())
  {
    char buff[SAFE_NAME_LEN];

    strmake(buff, str_start,
            MY_MIN(sizeof(buff)-1, length + (int) (str-str_start)));

    if (length == 0)
      push_warning_printf(thd, Sql_condition::WARN_LEVEL_WARN,
                          ER_NAME_BECOMES_EMPTY,
                          ER_THD(thd, ER_NAME_BECOMES_EMPTY),
                          buff);
    else
      push_warning_printf(thd, Sql_condition::WARN_LEVEL_WARN,
                          ER_REMOVED_SPACES, ER_THD(thd, ER_REMOVED_SPACES),
                          buff);
  }
  name= make_name(thd, str, length, cs, MAX_ALIAS_NAME - 1);
}


void Item::set_name_no_truncate(THD *thd, const char *str, uint length,
                                CHARSET_INFO *cs)
{
  name= make_name(thd, str, length, cs, UINT_MAX - 1);
}


/**
  @details
  This function is called when:
  - Comparing items in the WHERE clause (when doing where optimization)
  - When trying to find an ORDER BY/GROUP BY item in the SELECT part
*/

bool Item::eq(const Item *item, bool binary_cmp) const
{
  /*
    Note, that this is never TRUE if item is a Item_param:
    for all basic constants we have special checks, and Item_param's
    type() can be only among basic constant types.
  */
  return type() == item->type() && name.str && item->name.str &&
    !lex_string_cmp(system_charset_info, &name, &item->name);
}


Item *Item::safe_charset_converter(THD *thd, CHARSET_INFO *tocs)
{
  if (!needs_charset_converter(tocs))
    return this;
  Item_func_conv_charset *conv= new (thd->mem_root) Item_func_conv_charset(thd, this, tocs, 1);
  return conv && conv->safe ? conv : NULL;
}


/**
  Some pieces of the code do not support changing of
  Item_cache to other Item types.

  Example:
  Item_singlerow_subselect has "Item_cache **row".
  Creating of Item_func_conv_charset followed by THD::change_item_tree()
  should not change row[i] from Item_cache directly to Item_func_conv_charset,
  because Item_singlerow_subselect later calls Item_cache-specific methods,
  e.g. row[i]->store() and row[i]->cache_value().

  Let's wrap Item_func_conv_charset in a new Item_cache,
  so the Item_cache-specific methods can still be used for
  Item_singlerow_subselect::row[i] safely.

  As a bonus we cache the converted value, instead of converting every time

  TODO: we should eventually check all other use cases of change_item_tree().
  Perhaps some more potentially dangerous substitution examples exist.
*/

Item *Item_cache::safe_charset_converter(THD *thd, CHARSET_INFO *tocs)
{
  if (!example)
    return Item::safe_charset_converter(thd, tocs);
  Item *conv= example->safe_charset_converter(thd, tocs);
  if (conv == example)
    return this;
  Item_cache *cache;
  if (!conv || conv->fix_fields(thd, (Item **) NULL) ||
      unlikely(!(cache= new (thd->mem_root) Item_cache_str(thd, conv))))
    return NULL; // Safe conversion is not possible, or OEM
  cache->setup(thd, conv);
  return cache;
}


/**
  @details
  Created mostly for mysql_prepare_table(). Important
  when a string ENUM/SET column is described with a numeric default value:

  CREATE TABLE t1(a SET('a') DEFAULT 1);

  We cannot use generic Item::safe_charset_converter(), because
  the latter returns a non-fixed Item, so val_str() crashes afterwards.
  Override Item_num method, to return a fixed item.
*/

Item *Item_num::safe_charset_converter(THD *thd, CHARSET_INFO *tocs)
{
  /*
    Item_num returns pure ASCII result,
    so conversion is needed only in case of "tricky" character
    sets like UCS2. If tocs is not "tricky", return the item itself.
  */
  if (!(tocs->state & MY_CS_NONASCII))
    return this;
  
  Item *conv;
  if ((conv= const_charset_converter(thd, tocs, true)))
    conv->fix_char_length(max_char_length());
  return conv;
}


/**
  Create character set converter for constant items
  using Item_null, Item_string or Item_static_string_func.

  @param tocs       Character set to to convert the string to.
  @param lossless   Whether data loss is acceptable.
  @param func_name  Function name, or NULL.

  @return           this, if conversion is not needed,
                    NULL, if safe conversion is not possible, or
                    a new item representing the converted constant.
*/
Item *Item::const_charset_converter(THD *thd, CHARSET_INFO *tocs,
                                    bool lossless,
                                    const char *func_name)
{
  DBUG_ASSERT(const_item());
  DBUG_ASSERT(fixed());
  StringBuffer<64>tmp;
  String *s= val_str(&tmp);
  MEM_ROOT *mem_root= thd->mem_root;

  if (!s)
    return new (mem_root) Item_null(thd, (char *) func_name, tocs);

  if (!needs_charset_converter(s->length(), tocs))
  {
    if (collation.collation == &my_charset_bin && tocs != &my_charset_bin &&
        !this->check_well_formed_result(s, true))
      return NULL;
    return this;
  }

  uint conv_errors;
  Item_string *conv= (func_name ?
                      new (mem_root)
                      Item_static_string_func(thd, Lex_cstring_strlen(func_name),
                                              s, tocs, &conv_errors,
                                              collation.derivation,
                                              collation.repertoire) :
                      new (mem_root)
                      Item_string(thd, s, tocs, &conv_errors,
                                  collation.derivation,
                                  collation.repertoire));

  if (unlikely(!conv || (conv_errors && lossless)))
  {
    /*
      Safe conversion is not possible (or EOM).
      We could not convert a string into the requested character set
      without data loss. The target charset does not cover all the
      characters from the string. Operation cannot be done correctly.
    */
    return NULL;
  }
  if (s->charset() == &my_charset_bin && tocs != &my_charset_bin &&
      !conv->check_well_formed_result(true))
    return NULL;
  return conv;
}


Item *Item_param::safe_charset_converter(THD *thd, CHARSET_INFO *tocs)
{
  /*
    Return "this" if in prepare. result_type may change at execition time,
    to it's possible that the converter will not be needed at all:

    PREPARE stmt FROM 'SELECT * FROM t1 WHERE field = ?';
    SET @arg= 1;
    EXECUTE stmt USING @arg;

    In the above example result_type is STRING_RESULT at prepare time,
    and INT_RESULT at execution time.
  */
  return !const_item() || state == NULL_VALUE ?
         this : const_charset_converter(thd, tocs, true);
}


/**
  Get the value of the function as a MYSQL_TIME structure.
  As a extra convenience the time structure is reset on error or NULL values!
*/

bool Item::get_date_from_int(THD *thd, MYSQL_TIME *ltime, date_mode_t fuzzydate)
{
  Longlong_hybrid value(val_int(), unsigned_flag);
  return null_value || int_to_datetime_with_warn(thd, value,
                                                 ltime, fuzzydate,
                                                 field_table_or_null(),
                                                 field_name_or_null());
}


bool Item::get_date_from_real(THD *thd, MYSQL_TIME *ltime, date_mode_t fuzzydate)
{
  double value= val_real();
  return null_value || double_to_datetime_with_warn(thd, value,
                                                    ltime, fuzzydate,
                                                    field_table_or_null(),
                                                    field_name_or_null());
}


bool Item::get_date_from_string(THD *thd, MYSQL_TIME *to, date_mode_t mode)
{
  StringBuffer<MAX_DATETIME_FULL_WIDTH+1> tmp;
  const TABLE_SHARE *s = field_table_or_null();
  Temporal::Warn_push warn(thd, s ? s->db.str : nullptr,
                           s ? s->table_name.str : nullptr,
                           field_name_or_null(), to, mode);
  Temporal_hybrid *t= new(to) Temporal_hybrid(thd, &warn, val_str(&tmp), mode);
  return !t->is_valid_temporal();
}


const MY_LOCALE *Item::locale_from_val_str()
{
  StringBuffer<MAX_FIELD_WIDTH> tmp;
  String *locale_name= val_str_ascii(&tmp);
  const MY_LOCALE *lc;
  if (!locale_name ||
      !(lc= my_locale_by_name(locale_name->c_ptr_safe())))
  {
    THD *thd= current_thd;
    push_warning_printf(thd, Sql_condition::WARN_LEVEL_WARN,
                        ER_UNKNOWN_LOCALE,
                        ER_THD(thd, ER_UNKNOWN_LOCALE),
                        locale_name ? locale_name->c_ptr_safe() : "NULL");
    lc= &my_locale_en_US;
  }
  return lc;
}


CHARSET_INFO *Item::default_charset()
{
  return current_thd->variables.collation_connection;
}


/*
  Save value in field, but don't give any warnings

  NOTES
   This is used to temporary store and retrieve a value in a column,
   for example in opt_range to adjust the key value to fit the column.
*/

int Item::save_in_field_no_warnings(Field *field, bool no_conversions)
{
  int res;
  TABLE *table= field->table;
  THD *thd= table->in_use;
  enum_check_fields org_count_cuted_fields= thd->count_cuted_fields;
  sql_mode_t org_sql_mode= thd->variables.sql_mode;
  MY_BITMAP *old_map= dbug_tmp_use_all_columns(table, &table->write_set);

  thd->variables.sql_mode&= ~(MODE_NO_ZERO_IN_DATE | MODE_NO_ZERO_DATE);
  thd->variables.sql_mode|= MODE_INVALID_DATES;
  thd->count_cuted_fields= CHECK_FIELD_IGNORE;

  res= save_in_field(field, no_conversions);

  thd->count_cuted_fields= org_count_cuted_fields;
  thd->variables.sql_mode= org_sql_mode;
  dbug_tmp_restore_column_map(&table->write_set, old_map);
  return res;
}


#ifndef DBUG_OFF
static inline
void mark_unsupported_func(const char *where, const char *processor_name)
{
  char buff[64];
  my_snprintf(buff, sizeof(buff), "%s::%s", where ? where: "", processor_name);
  DBUG_ENTER(buff);
  my_snprintf(buff, sizeof(buff), "%s returns TRUE: unsupported function", processor_name);
  DBUG_PRINT("info", ("%s", buff));
  DBUG_VOID_RETURN;
}
#else
#define mark_unsupported_func(X,Y) {}
#endif

bool mark_unsupported_function(const char *where, void *store, uint result)
{
  Item::vcol_func_processor_result *res=
    (Item::vcol_func_processor_result*) store;
  uint old_errors= res->errors;
  mark_unsupported_func(where, "check_vcol_func_processor");
  res->errors|= result;  /* Store type of expression */
  /* Store the name to the highest violation (normally VCOL_IMPOSSIBLE) */
  if (result > old_errors)
    res->name= where ? where : "";
  return false;
}

/* convenience helper for mark_unsupported_function() above */
bool mark_unsupported_function(const char *w1, const char *w2,
                               void *store, uint result)
{
  char *ptr= (char*)current_thd->alloc(strlen(w1) + strlen(w2) + 1);
  if (ptr)
    strxmov(ptr, w1, w2, NullS);
  return mark_unsupported_function(ptr, store, result);
}


bool Item_field::check_vcol_func_processor(void *arg)
{
  context= 0;
  vcol_func_processor_result *res= (vcol_func_processor_result *) arg;
  if (res && res->alter_info)
  {
    for (Key &k: res->alter_info->key_list)
    {
      if (k.type != Key::FOREIGN_KEY)
        continue;
      Foreign_key *fk= (Foreign_key*) &k;
      if (fk->update_opt != FK_OPTION_CASCADE)
        continue;
      for (Key_part_spec& kp: fk->columns)
      {
        if (!lex_string_cmp(system_charset_info, &kp.field_name, &field_name))
        {
          return mark_unsupported_function(field_name.str, arg, VCOL_IMPOSSIBLE);
        }
      }
    }
  }
  if (field && (field->unireg_check == Field::NEXT_NUMBER))
  {
    // Auto increment fields are unsupported
    return mark_unsupported_function(field_name.str, arg, VCOL_FIELD_REF | VCOL_AUTO_INC);
  }
  return mark_unsupported_function(field_name.str, arg, VCOL_FIELD_REF);
}


Query_fragment::Query_fragment(THD *thd, sp_head *sphead,
                               const char *start, const char *end)
{
  DBUG_ASSERT(start <= end);
  if (thd->lex->clone_spec_offset)
  {
    Lex_input_stream *lip= (& thd->m_parser_state->m_lip);
    DBUG_ASSERT(lip->get_buf() <= start);
    DBUG_ASSERT(end <= lip->get_end_of_query());
    set(start - lip->get_buf(), end - start);
  }
  else if (sphead)
  {
    if (sphead->m_tmp_query)
    {
      // Normal SP statement
      DBUG_ASSERT(sphead->m_tmp_query <= start);
      set(start - sphead->m_tmp_query, end - start);
    }
    else
    {
      /*
        We're in the "if" expression of a compound query:
          if (expr)
            do_something;
          end if;
        sphead->m_tmp_query is not set yet at this point, because
        the "if" part of such statements is never put into the binary log.
        Values of Rewritable_query_parameter::pos_in_query and
        Rewritable_query_parameter:len_in_query will not be important,
        so setting both to 0 should be fine.
      */
      set(0, 0);
    }
  }
  else
  {
    // Non-SP statement
    DBUG_ASSERT(thd->query() <= start);
    DBUG_ASSERT(end <= thd->query_end());
    set(start - thd->query(), end - start);
  }
}


/*****************************************************************************
  Item_sp_variable methods
*****************************************************************************/

Item_sp_variable::Item_sp_variable(THD *thd, const LEX_CSTRING *sp_var_name)
  :Item_fixed_hybrid(thd), m_thd(0), m_name(*sp_var_name)
#ifndef DBUG_OFF
   , m_sp(0)
#endif
{
}


bool Item_sp_variable::fix_fields_from_item(THD *thd, Item **, const Item *it)
{
  m_thd= thd; /* NOTE: this must be set before any this_xxx() */

  DBUG_ASSERT(it->fixed());

  max_length= it->max_length;
  decimals= it->decimals;
  unsigned_flag= it->unsigned_flag;
  base_flags|= item_base_t::FIXED;
  with_flags|= item_with_t::SP_VAR;
  if (thd->lex->current_select && thd->lex->current_select->master_unit()->item)
    thd->lex->current_select->master_unit()->item->with_flags|= item_with_t::SP_VAR;
  collation.set(it->collation.collation, it->collation.derivation);

  return FALSE;
}


double Item_sp_variable::val_real()
{
  DBUG_ASSERT(fixed());
  Item *it= this_item();
  double ret= it->val_real();
  null_value= it->null_value;
  return ret;
}


longlong Item_sp_variable::val_int()
{
  DBUG_ASSERT(fixed());
  Item *it= this_item();
  longlong ret= it->val_int();
  null_value= it->null_value;
  return ret;
}


String *Item_sp_variable::val_str(String *sp)
{
  DBUG_ASSERT(fixed());
  Item *it= this_item();
  String *res= it->val_str(sp);

  null_value= it->null_value;

  if (!res)
    return NULL;

  /*
    This way we mark returned value of val_str as const,
    so that various functions (e.g. CONCAT) won't try to
    modify the value of the Item. Analogous mechanism is
    implemented for Item_param.
    Without this trick Item_splocal could be changed as a
    side-effect of expression computation. Here is an example
    of what happens without it: suppose x is varchar local
    variable in a SP with initial value 'ab' Then
      select concat(x,'c');
    would change x's value to 'abc', as Item_func_concat::val_str()
    would use x's internal buffer to compute the result.
    This is intended behaviour of Item_func_concat. Comments to
    Item_param class contain some more details on the topic.
  */

  if (res != &str_value)
    str_value.set(res->ptr(), res->length(), res->charset());
  else
    res->mark_as_const();

  return &str_value;
}


bool Item_sp_variable::val_native(THD *thd, Native *to)
{
  return val_native_from_item(thd, this_item(), to);
}


my_decimal *Item_sp_variable::val_decimal(my_decimal *decimal_value)
{
  DBUG_ASSERT(fixed());
  Item *it= this_item();
  my_decimal *val= it->val_decimal(decimal_value);
  null_value= it->null_value;
  return val;
}


bool Item_sp_variable::get_date(THD *thd, MYSQL_TIME *ltime, date_mode_t fuzzydate)
{
  DBUG_ASSERT(fixed());
  Item *it= this_item();
  bool val= it->get_date(thd, ltime, fuzzydate);
  null_value= it->null_value;
  return val;
}


bool Item_sp_variable::is_null()
{
  return this_item()->is_null();
}

void Item_sp_variable::make_send_field(THD *thd, Send_field *field)
{
  Item *it= this_item();

  it->make_send_field(thd, field);
  if (name.str)
    field->col_name= name;
  else
    field->col_name= m_name;
}

/*****************************************************************************
  Item_splocal methods
*****************************************************************************/

Item_splocal::Item_splocal(THD *thd,
                           const Sp_rcontext_handler *rh,
                           const LEX_CSTRING *sp_var_name,
                           uint sp_var_idx,
                           const Type_handler *handler,
                           uint pos_in_q, uint len_in_q):
  Item_sp_variable(thd, sp_var_name),
  Rewritable_query_parameter(pos_in_q, len_in_q),
  Type_handler_hybrid_field_type(handler),
  m_rcontext_handler(rh),
  m_var_idx(sp_var_idx),
  m_type(handler == &type_handler_row ? ROW_ITEM : CONST_ITEM)
{
  set_maybe_null();
}


sp_rcontext *Item_splocal::get_rcontext(sp_rcontext *local_ctx) const
{
  return m_rcontext_handler->get_rcontext(local_ctx);
}


Item_field *Item_splocal::get_variable(sp_rcontext *ctx) const
{
  return get_rcontext(ctx)->get_variable(m_var_idx);
}


bool Item_splocal::fix_fields(THD *thd, Item **ref)
{
  DBUG_ASSERT(fixed() == 0);
  Item *item= get_variable(thd->spcont);
  set_handler(item->type_handler());
  return fix_fields_from_item(thd, ref, item);
}


Item *
Item_splocal::this_item()
{
  DBUG_ASSERT(m_sp == m_thd->spcont->m_sp);
  DBUG_ASSERT(fixed());
  return get_variable(m_thd->spcont);
}

const Item *
Item_splocal::this_item() const
{
  DBUG_ASSERT(m_sp == m_thd->spcont->m_sp);
  DBUG_ASSERT(fixed());
  return get_variable(m_thd->spcont);
}


Item **
Item_splocal::this_item_addr(THD *thd, Item **)
{
  DBUG_ASSERT(m_sp == thd->spcont->m_sp);
  DBUG_ASSERT(fixed());
  return get_rcontext(thd->spcont)->get_variable_addr(m_var_idx);
}


void Item_splocal::print(String *str, enum_query_type)
{
  const LEX_CSTRING *prefix= m_rcontext_handler->get_name_prefix();
  str->reserve(m_name.length + 8 + prefix->length);
  str->append(prefix);
  str->append(&m_name);
  str->append('@');
  str->qs_append(m_var_idx);
}


bool Item_splocal::set_value(THD *thd, sp_rcontext *ctx, Item **it)
{
  return get_rcontext(ctx)->set_variable(thd, get_var_idx(), it);
}


/**
  These two declarations are different:
    x INT;
    ROW(x INT);
  A ROW with one elements should not be comparable to scalar value.

  TODO: Currently we don't support one argument with the function ROW(), so
  this query returns a syntax error, meaning that more arguments are expected:
    SELECT ROW(1);

  Therefore, all around the code we assume that cols()==1 means a scalar value
  and cols()>1 means a ROW value. With adding ROW SP variables this
  assumption is not true any more. ROW variables with one element are
  now possible.

  To implement Item::check_cols() correctly, we now should extend it to
  know if a ROW or a scalar value is being tested. For example,
  these new prototypes should work:
    virtual bool check_cols(Item_result result, uint c);
  or
    virtual bool check_cols(const Type_handler *type, uint c);

  The current implementation of Item_splocal::check_cols() is a compromise
  that should be more or less fine until we extend check_cols().
  It disallows ROW variables to appear in a scalar context.
  The "|| n == 1" part of the conditon is responsible for this.
  For example, it disallows ROW variables to appear in SELECT list:

DELIMITER $$;
CREATE PROCEDURE p1()
AS
  a ROW (a INT);
BEGIN
  SELECT a;
END;
$$
DELIMITER ;$$
--error ER_OPERAND_COLUMNS
CALL p1();

  But is produces false negatives with ROW variables consisting of one element.
  For example, this script fails:

SET sql_mode=ORACLE;
DROP PROCEDURE IF EXISTS p1;
DELIMITER $$
CREATE PROCEDURE p1
AS
  a ROW(a INT);
  b ROW(a INT);
BEGIN
  SELECT a=b;
END;
$$
DELIMITER ;
CALL p1();

  and returns "ERROR 1241 (21000): Operand should contain 1 column(s)".
  This will be fixed that we change check_cols().
*/

bool Item_splocal::check_cols(uint n)
{
  DBUG_ASSERT(m_thd->spcont);
  if (Type_handler_hybrid_field_type::cmp_type() != ROW_RESULT)
    return Item::check_cols(n);

  if (n != this_item()->cols() || n == 1)
  {
    my_error(ER_OPERAND_COLUMNS, MYF(0), n);
    return true;
  }
  return false;
}


bool Item_splocal_row_field::fix_fields(THD *thd, Item **ref)
{
  DBUG_ASSERT(fixed() == 0);
  Item *item= get_variable(thd->spcont)->element_index(m_field_idx);
  return fix_fields_from_item(thd, ref, item);
}


Item *
Item_splocal_row_field::this_item()
{
  DBUG_ASSERT(m_sp == m_thd->spcont->m_sp);
  DBUG_ASSERT(fixed());
  return get_variable(m_thd->spcont)->element_index(m_field_idx);
}


const Item *
Item_splocal_row_field::this_item() const
{
  DBUG_ASSERT(m_sp == m_thd->spcont->m_sp);
  DBUG_ASSERT(fixed());
  return get_variable(m_thd->spcont)->element_index(m_field_idx);
}


Item **
Item_splocal_row_field::this_item_addr(THD *thd, Item **)
{
  DBUG_ASSERT(m_sp == thd->spcont->m_sp);
  DBUG_ASSERT(fixed());
  return get_variable(thd->spcont)->addr(m_field_idx);
}


void Item_splocal_row_field::print(String *str, enum_query_type)
{
  const LEX_CSTRING *prefix= m_rcontext_handler->get_name_prefix();
  str->reserve(m_name.length + m_field_name.length + 8 + prefix->length);
  str->append(prefix);
  str->append(&m_name);
  str->append('.');
  str->append(&m_field_name);
  str->append('@');
  str->qs_append(m_var_idx);
  str->append('[');
  str->qs_append(m_field_idx);
  str->append(']');
}


bool Item_splocal_row_field::set_value(THD *thd, sp_rcontext *ctx, Item **it)
{
  return get_rcontext(ctx)->set_variable_row_field(thd, m_var_idx, m_field_idx,
                                                   it);
}


bool Item_splocal_row_field_by_name::fix_fields(THD *thd, Item **it)
{
  DBUG_ASSERT(fixed() == 0);
  m_thd= thd;
  if (get_rcontext(thd->spcont)->find_row_field_by_name_or_error(&m_field_idx,
                                                                 m_var_idx,
                                                                 m_field_name))
    return true;
  Item *item= get_variable(thd->spcont)->element_index(m_field_idx);
  set_handler(item->type_handler());
  return fix_fields_from_item(thd, it, item);
}


void Item_splocal_row_field_by_name::print(String *str, enum_query_type)
{
  const LEX_CSTRING *prefix= m_rcontext_handler->get_name_prefix();
  // +16 should be enough for .NNN@[""]
  if (str->reserve(m_name.length + 2 * m_field_name.length +
                   prefix->length + 16))
    return;
  str->qs_append(prefix);
  str->qs_append(&m_name);
  str->qs_append('.');
  str->qs_append(&m_field_name);
  str->qs_append('@');
  str->qs_append(m_var_idx);
  str->qs_append("[\"", 2);
  str->qs_append(&m_field_name);
  str->qs_append("\"]", 2);
}


bool Item_splocal_row_field_by_name::set_value(THD *thd, sp_rcontext *ctx, Item **it)
{
  DBUG_ASSERT(fixed()); // Make sure m_field_idx is already set
  return Item_splocal_row_field::set_value(thd, ctx, it);
}


/*****************************************************************************
  Item_case_expr methods
*****************************************************************************/

LEX_CSTRING str_case_expr= { STRING_WITH_LEN("case_expr") };

Item_case_expr::Item_case_expr(THD *thd, uint case_expr_id):
  Item_sp_variable(thd, &str_case_expr),
  m_case_expr_id(case_expr_id)
{
}


bool Item_case_expr::fix_fields(THD *thd, Item **ref)
{
  Item *item= thd->spcont->get_case_expr(m_case_expr_id);
  return fix_fields_from_item(thd, ref, item);
}


Item *
Item_case_expr::this_item()
{
  DBUG_ASSERT(m_sp == m_thd->spcont->m_sp);

  return m_thd->spcont->get_case_expr(m_case_expr_id);
}



const Item *
Item_case_expr::this_item() const
{
  DBUG_ASSERT(m_sp == m_thd->spcont->m_sp);

  return m_thd->spcont->get_case_expr(m_case_expr_id);
}


Item **
Item_case_expr::this_item_addr(THD *thd, Item **)
{
  DBUG_ASSERT(m_sp == thd->spcont->m_sp);

  return thd->spcont->get_case_expr_addr(m_case_expr_id);
}


void Item_case_expr::print(String *str, enum_query_type)
{
  if (str->reserve(MAX_INT_WIDTH + sizeof("case_expr@")))
    return;                                    /* purecov: inspected */
  (void) str->append(STRING_WITH_LEN("case_expr@"));
  str->qs_append(m_case_expr_id);
}


/*****************************************************************************
  Item_name_const methods
*****************************************************************************/

double Item_name_const::val_real()
{
  DBUG_ASSERT(fixed());
  double ret= value_item->val_real();
  null_value= value_item->null_value;
  return ret;
}


longlong Item_name_const::val_int()
{
  DBUG_ASSERT(fixed());
  longlong ret= value_item->val_int();
  null_value= value_item->null_value;
  return ret;
}


String *Item_name_const::val_str(String *sp)
{
  DBUG_ASSERT(fixed());
  String *ret= value_item->val_str(sp);
  null_value= value_item->null_value;
  return ret;
}


my_decimal *Item_name_const::val_decimal(my_decimal *decimal_value)
{
  DBUG_ASSERT(fixed());
  my_decimal *val= value_item->val_decimal(decimal_value);
  null_value= value_item->null_value;
  return val;
}

bool Item_name_const::get_date(THD *thd, MYSQL_TIME *ltime, date_mode_t fuzzydate)
{
  DBUG_ASSERT(fixed());
  bool rc= value_item->get_date(thd, ltime, fuzzydate);
  null_value= value_item->null_value;
  return rc;
}

bool Item_name_const::val_native(THD *thd, Native *to)
{
  return val_native_from_item(thd, value_item, to);
}

bool Item_name_const::is_null()
{
  return value_item->is_null();
}


Item_name_const::Item_name_const(THD *thd, Item *name_arg, Item *val):
  Item_fixed_hybrid(thd), value_item(val), name_item(name_arg)
{
  StringBuffer<128> name_buffer;
  String *name_str;

  set_maybe_null();
  if (name_item->basic_const_item() &&
      (name_str= name_item->val_str(&name_buffer))) // Can't have a NULL name
    set_name(thd, name_str);
}


Item::Type Item_name_const::type() const
{
  /*

    We are guarenteed that value_item->basic_const_item(), if not
    an error is thrown that WRONG ARGUMENTS are supplied to
    NAME_CONST function.
    If type is FUNC_ITEM, then we have a fudged item_func_neg()
    on our hands and return the underlying type.
    For Item_func_set_collation()
    e.g. NAME_CONST('name', 'value' COLLATE collation) we return its
    'value' argument type. 
  */
  Item::Type value_type= value_item->type();
  if (value_type == FUNC_ITEM)
  {
    /* 
      The second argument of NAME_CONST('name', 'value') must be 
      a simple constant item or a NEG_FUNC/COLLATE_FUNC.
    */
    DBUG_ASSERT(((Item_func *) value_item)->functype() == 
                Item_func::NEG_FUNC ||
                ((Item_func *) value_item)->functype() == 
                Item_func::COLLATE_FUNC);
    return ((Item_func *) value_item)->key_item()->type();            
  }
  return value_type;
}


bool Item_name_const::fix_fields(THD *thd, Item **ref)
{
  if (value_item->fix_fields_if_needed(thd, &value_item) ||
      name_item->fix_fields_if_needed(thd, &name_item) ||
      !value_item->const_item() ||
      !name_item->const_item())
  {
    my_error(ER_RESERVED_SYNTAX, MYF(0), "NAME_CONST");
    return TRUE;
  }
  if (value_item->collation.derivation == DERIVATION_NUMERIC)
    collation= DTCollation_numeric();
  else
    collation.set(value_item->collation.collation, DERIVATION_IMPLICIT);
  max_length= value_item->max_length;
  decimals= value_item->decimals;
  unsigned_flag= value_item->unsigned_flag;
  base_flags|= item_base_t::FIXED;
  return FALSE;
}


void Item_name_const::print(String *str, enum_query_type query_type)
{
  str->append(STRING_WITH_LEN("NAME_CONST("));
  name_item->print(str, query_type);
  str->append(',');
  value_item->print(str, query_type);
  str->append(')');
}


/*
 need a special class to adjust printing : references to aggregate functions 
 must not be printed as refs because the aggregate functions that are added to
 the front of select list are not printed as well.
*/
class Item_aggregate_ref : public Item_ref
{
public:
  Item_aggregate_ref(THD *thd, Name_resolution_context *context_arg,
                     Item **item, const LEX_CSTRING &table_name_arg,
                     const LEX_CSTRING &field_name_arg):
    Item_ref(thd, context_arg, item, table_name_arg, field_name_arg) {}

  void print (String *str, enum_query_type query_type) override
  {
    if (ref)
      (*ref)->print(str, query_type);
    else
      Item_ident::print(str, query_type);
  }
  Ref_Type ref_type() override final { return AGGREGATE_REF; }
};


/**
  Move SUM items out from item tree and replace with reference.

  @param thd			Thread handler
  @param ref_pointer_array	Pointer to array of reference fields
  @param fields		        All fields in select
  @param ref			Pointer to item
  @param split_flags            Zero or more of the following flags
	                        SPLIT_FUNC_SKIP_REGISTERED:
                                Function be must skipped for registered SUM
                                SUM items
                                SPLIT_SUM_SELECT
                                We are called on the select level and have to
                                register items operated on sum function

  @note
    All found SUM items are added FIRST in the fields list and
    we replace the item with a reference.

    If this is an item in the SELECT list then we also have to split out
    all arguments to functions used together with the sum function.
    For example in case of SELECT A*sum(B) we have to split out both
    A and sum(B).
    This is not needed for ORDER BY, GROUP BY or HAVING as all references
    to items in the select list are already of type REF

    thd->fatal_error() may be called if we are out of memory
*/

void Item::split_sum_func2(THD *thd, Ref_ptr_array ref_pointer_array,
                           List<Item> &fields, Item **ref, 
                           uint split_flags)
{
  if (unlikely(type() == SUM_FUNC_ITEM))
  {
    /* An item of type Item_sum is registered if ref_by != 0 */
    if ((split_flags & SPLIT_SUM_SKIP_REGISTERED) && 
        ((Item_sum *) this)->ref_by)
      return;
  }
  else if (type() == WINDOW_FUNC_ITEM || with_window_func())
  {
    /*
      Skip the else part, window functions are very special functions: 
      they need to have their own fields in the temp. table, but they
      need to be proceessed differently than regular aggregate functions

      Call split_sum_func here so that each argument gets its fields to
      point to the temporary table.
    */
    split_sum_func(thd, ref_pointer_array, fields, split_flags);
    if (type() == FUNC_ITEM) {
      return;
    }
  }
  else if (type() == FUNC_ITEM &&
           ((Item_func*)this)->functype() == Item_func::ROWNUM_FUNC)
  {
  }
  else
  {
    /* Not a SUM() function */
    if (!with_sum_func() && !with_rownum_func() &&
        !(split_flags & SPLIT_SUM_SELECT))
    {
      /*
        This is not a SUM function and there are no SUM functions inside.
        Nothing more to do.
      */
      return;
    }
    if (likely(with_sum_func() ||
               (type() == FUNC_ITEM &&
                (((Item_func *) this)->functype() ==
                 Item_func::ISNOTNULLTEST_FUNC ||
                 ((Item_func *) this)->functype() ==
                 Item_func::TRIG_COND_FUNC))))
    {
      /* Will call split_sum_func2() for all items */
      split_sum_func(thd, ref_pointer_array, fields, split_flags);
      return;
    }

    if (unlikely((!(used_tables() & ~PARAM_TABLE_BIT) ||
                  (type() == REF_ITEM &&
                   ((Item_ref*)this)->ref_type() != Item_ref::VIEW_REF))))
        return;
  }

  /*
    Replace item with a reference so that we can easily calculate
    it (in case of sum functions) or copy it (in case of fields)

    The test above is to ensure we don't do a reference for things
    that are constants (PARAM_TABLE_BIT is in effect a constant)
    or already referenced (for example an item in HAVING)
    Exception is Item_direct_view_ref which we need to convert to
    Item_ref to allow fields from view being stored in tmp table.
  */
  Item_ref *item_ref;
  uint el= fields.elements;
  /*
    If this is an item_ref, get the original item
    This is a safety measure if this is called for things that is
    already a reference.
  */
  Item *real_itm= real_item();
  ref_pointer_array[el]= real_itm;
  if (type() == WINDOW_FUNC_ITEM)
  {
    if (!(item_ref= (new (thd->mem_root)
                     Item_direct_ref(thd,
                                     &thd->lex->current_select->context,
                                     &ref_pointer_array[el],
                                     null_clex_str, name))))
      return;                                   // fatal_error is set
  }
  else
  {
    if (!(item_ref= (new (thd->mem_root)
                     Item_aggregate_ref(thd,
                                        &thd->lex->current_select->context,
                                        &ref_pointer_array[el],
                                        null_clex_str, name))))
      return;                                   // fatal_error is set
  }
  if (type() == SUM_FUNC_ITEM)
    item_ref->depended_from= ((Item_sum *) this)->depended_from();
  fields.push_front(real_itm);
  thd->change_item_tree(ref, item_ref);
}


static bool
left_is_superset(const DTCollation *left, const DTCollation *right)
{
  /* Allow convert to Unicode */
  if (left->collation->state & MY_CS_UNICODE &&
      (left->derivation < right->derivation ||
       (left->derivation == right->derivation &&
        (!(right->collation->state & MY_CS_UNICODE) ||
         /* The code below makes 4-byte utf8 a superset over 3-byte utf8 */
         (left->collation->state & MY_CS_UNICODE_SUPPLEMENT &&
          !(right->collation->state & MY_CS_UNICODE_SUPPLEMENT) &&
          left->collation->mbmaxlen > right->collation->mbmaxlen &&
          left->collation->mbminlen == right->collation->mbminlen)))))
    return TRUE;
  /* Allow convert from ASCII */
  if (right->repertoire == MY_REPERTOIRE_ASCII &&
      (left->derivation < right->derivation ||
       (left->derivation == right->derivation &&
        !(left->repertoire == MY_REPERTOIRE_ASCII))))
    return TRUE;
  /* Disallow conversion otherwise */
  return FALSE;
}

/**
  Aggregate two collations together taking
  into account their coercibility (aka derivation):.

  0 == DERIVATION_EXPLICIT  - an explicitly written COLLATE clause @n
  1 == DERIVATION_NONE      - a mix of two different collations @n
  2 == DERIVATION_IMPLICIT  - a column @n
  3 == DERIVATION_COERCIBLE - a string constant.

  The most important rules are:
  -# If collations are the same:
  chose this collation, and the strongest derivation.
  -# If collations are different:
  - Character sets may differ, but only if conversion without
  data loss is possible. The caller provides flags whether
  character set conversion attempts should be done. If no
  flags are substituted, then the character sets must be the same.
  Currently processed flags are:
  MY_COLL_ALLOW_SUPERSET_CONV  - allow conversion to a superset
  MY_COLL_ALLOW_COERCIBLE_CONV - allow conversion of a coercible value
  - two EXPLICIT collations produce an error, e.g. this is wrong:
  CONCAT(expr1 collate latin1_swedish_ci, expr2 collate latin1_german_ci)
  - the side with smaller derivation value wins,
  i.e. a column is stronger than a string constant,
  an explicit COLLATE clause is stronger than a column.
  - if derivations are the same, we have DERIVATION_NONE,
  we'll wait for an explicit COLLATE clause which possibly can
  come from another argument later: for example, this is valid,
  but we don't know yet when collecting the first two arguments:
     @code
       CONCAT(latin1_swedish_ci_column,
              latin1_german1_ci_column,
              expr COLLATE latin1_german2_ci)
  @endcode
*/

bool DTCollation::aggregate(const DTCollation &dt, uint flags)
{
  if (!my_charset_same(collation, dt.collation))
  {
    /* 
       We do allow to use binary strings (like BLOBS)
       together with character strings.
       Binaries have more precedence than a character
       string of the same derivation.
    */
    if (collation == &my_charset_bin)
    {
      if (derivation <= dt.derivation)
      {
	/* Do nothing */
      }
      else
      {
	set(dt); 
      }
    }
    else if (dt.collation == &my_charset_bin)
    {
      if (dt.derivation <= derivation)
      {
        set(dt);
      }
    }
    else if ((flags & MY_COLL_ALLOW_SUPERSET_CONV) &&
             left_is_superset(this, &dt))
    {
      /* Do nothing */
    }
    else if ((flags & MY_COLL_ALLOW_SUPERSET_CONV) &&
             left_is_superset(&dt, this))
    {
      set(dt);
    }
    else if ((flags & MY_COLL_ALLOW_COERCIBLE_CONV) &&
             derivation < dt.derivation &&
             dt.derivation >= DERIVATION_SYSCONST)
    {
      /* Do nothing */
    }
    else if ((flags & MY_COLL_ALLOW_COERCIBLE_CONV) &&
             dt.derivation < derivation &&
             derivation >= DERIVATION_SYSCONST)
    {
      set(dt);
    }
    else
    {
      // Cannot apply conversion
      set(&my_charset_bin, DERIVATION_NONE,
          (dt.repertoire|repertoire));
      return 1;
    }
  }
  else if (derivation < dt.derivation)
  {
    /* Do nothing */
  }
  else if (dt.derivation < derivation)
  {
    set(dt);
  }
  else
  { 
    if (collation == dt.collation)
    {
      /* Do nothing */
    }
    else 
    {
      if (derivation == DERIVATION_EXPLICIT)
      {
        set(0, DERIVATION_NONE, MY_REPERTOIRE_NONE);
        return 1;
      }
      if (collation->state & MY_CS_BINSORT &&
          dt.collation->state & MY_CS_BINSORT)
        return 1;
      if (collation->state & MY_CS_BINSORT)
        return 0;
      if (dt.collation->state & MY_CS_BINSORT)
      {
        set(dt);
        return 0;
      }
      THD *thd = current_thd;
      myf utf8_flag= thd ? thd->get_utf8_flag()
        : global_system_variables.old_behavior & OLD_MODE_UTF8_IS_UTF8MB3;
      CHARSET_INFO *bin= get_charset_by_csname(collation->cs_name.str,
                                               MY_CS_BINSORT,MYF(utf8_flag));
      set(bin, DERIVATION_NONE);
    }
  }
  repertoire|= dt.repertoire;
  return 0;
}

/******************************/
static
void my_coll_agg_error(DTCollation &c1, DTCollation &c2, const char *fname)
{
  my_error(ER_CANT_AGGREGATE_2COLLATIONS,MYF(0),
           c1.collation->coll_name.str, c1.derivation_name(),
           c2.collation->coll_name.str, c2.derivation_name(),
           fname);
}


static
void my_coll_agg_error(DTCollation &c1, DTCollation &c2, DTCollation &c3,
                       const char *fname)
{
  my_error(ER_CANT_AGGREGATE_3COLLATIONS,MYF(0),
           c1.collation->coll_name.str, c1.derivation_name(),
           c2.collation->coll_name.str, c2.derivation_name(),
           c3.collation->coll_name.str, c3.derivation_name(),
           fname);
}


static
void my_coll_agg_error(Item** args, uint count, const char *fname,
                       int item_sep)
{
  if (count == 2)
    my_coll_agg_error(args[0]->collation, args[item_sep]->collation, fname);
  else if (count == 3)
    my_coll_agg_error(args[0]->collation, args[item_sep]->collation,
                      args[2*item_sep]->collation, fname);
  else
    my_error(ER_CANT_AGGREGATE_NCOLLATIONS,MYF(0),fname);
}


bool Type_std_attributes::agg_item_collations(DTCollation &c,
                                              const LEX_CSTRING &fname,
                                              Item **av, uint count,
                                              uint flags, int item_sep)
{
  uint i;
  Item **arg;
  bool unknown_cs= 0;

  c.set(av[0]->collation);
  for (i= 1, arg= &av[item_sep]; i < count; i++, arg+= item_sep)
  {
    if (c.aggregate((*arg)->collation, flags))
    {
      if (c.derivation == DERIVATION_NONE &&
          c.collation == &my_charset_bin)
      {
        unknown_cs= 1;
        continue;
      }
      my_coll_agg_error(av, count, fname.str, item_sep);
      return TRUE;
    }
  }

  if (unknown_cs &&
      c.derivation != DERIVATION_EXPLICIT)
  {
    my_coll_agg_error(av, count, fname.str, item_sep);
    return TRUE;
  }

  if ((flags & MY_COLL_DISALLOW_NONE) &&
      c.derivation == DERIVATION_NONE)
  {
    my_coll_agg_error(av, count, fname.str, item_sep);
    return TRUE;
  }
  
  /* If all arguments where numbers, reset to @@collation_connection */
  if (flags & MY_COLL_ALLOW_NUMERIC_CONV &&
      c.derivation == DERIVATION_NUMERIC)
    c.set(Item::default_charset(), DERIVATION_COERCIBLE, MY_REPERTOIRE_NUMERIC);

  return FALSE;
}


bool Type_std_attributes::agg_item_set_converter(const DTCollation &coll,
                                                 const LEX_CSTRING &fname,
                                                 Item **args, uint nargs,
                                                 uint flags, int item_sep)
{
  THD *thd= current_thd;
  if (thd->lex->is_ps_or_view_context_analysis())
    return false;
  Item **arg, *safe_args[2]= {NULL, NULL};

  /*
    For better error reporting: save the first and the second argument.
    We need this only if the the number of args is 3 or 2:
    - for a longer argument list, "Illegal mix of collations"
      doesn't display each argument's characteristics.
    - if nargs is 1, then this error cannot happen.
  */
  if (nargs >=2 && nargs <= 3)
  {
    safe_args[0]= args[0];
    safe_args[1]= args[item_sep];
  }

  bool res= FALSE;
  uint i;

  DBUG_ASSERT(!thd->stmt_arena->is_stmt_prepare());

  for (i= 0, arg= args; i < nargs; i++, arg+= item_sep)
  {
    Item* conv= (*arg)->safe_charset_converter(thd, coll.collation);
    if (conv == *arg)
      continue;

    if (!conv)
    {
      if (nargs >=2 && nargs <= 3)
      {
        /* restore the original arguments for better error message */
        args[0]= safe_args[0];
        args[item_sep]= safe_args[1];
      }
      my_coll_agg_error(args, nargs, fname.str, item_sep);
      res= TRUE;
      break; // we cannot return here, we need to restore "arena".
    }

    thd->change_item_tree(arg, conv);

    if (conv->fix_fields_if_needed(thd, arg))
    {
      res= TRUE;
      break; // we cannot return here, we need to restore "arena".
    }
  }
  return res;
}


/**
  @brief
    Building clone for Item_func_or_sum

  @param thd        thread handle
  @param mem_root   part of the memory for the clone

  @details
    This method first builds clones of the arguments. If it is successful with
    buiding the clones then it constructs a copy of this Item_func_or_sum object
    and attaches to it the built clones of the arguments.

   @return clone of the item
   @retval 0 on a failure
*/

Item* Item_func_or_sum::build_clone(THD *thd)
{
  Item *copy_tmp_args[2]= {0,0};
  Item **copy_args= copy_tmp_args;
  if (arg_count > 2)
  {
    copy_args= static_cast<Item**>
      (alloc_root(thd->mem_root, sizeof(Item*) * arg_count));
    if (unlikely(!copy_args))
      return 0;
  }
  for (uint i= 0; i < arg_count; i++)
  {
    Item *arg_clone= args[i]->build_clone(thd);
    if (unlikely(!arg_clone))
      return 0;
    copy_args[i]= arg_clone;
  }
  Item_func_or_sum *copy= static_cast<Item_func_or_sum *>(get_copy(thd));
  if (unlikely(!copy))
    return 0;
  if (arg_count > 2)
    copy->args= copy_args;
  else if (arg_count > 0)
  {
    copy->args= copy->tmp_arg;
    memcpy(copy->args, copy_args, sizeof(Item *) * arg_count);
  }
  return copy;
}

Item_sp::Item_sp(THD *thd, Name_resolution_context *context_arg,
                 sp_name *name_arg) :
  context(context_arg), m_name(name_arg), m_sp(NULL), func_ctx(NULL),
  sp_result_field(NULL)
{
  dummy_table= (TABLE*) thd->calloc(sizeof(TABLE) + sizeof(TABLE_SHARE) +
                                    sizeof(Query_arena));
  dummy_table->s= (TABLE_SHARE*) (dummy_table + 1);
  sp_query_arena= new(dummy_table->s + 1) Query_arena();
  memset(&sp_mem_root, 0, sizeof(sp_mem_root));
}

Item_sp::Item_sp(THD *thd, Item_sp *item):
         context(item->context), m_name(item->m_name),
         m_sp(item->m_sp), func_ctx(NULL), sp_result_field(NULL)
{
  dummy_table= (TABLE*) thd->calloc(sizeof(TABLE)+ sizeof(TABLE_SHARE) +
                                    sizeof(Query_arena));
  dummy_table->s= (TABLE_SHARE*) (dummy_table+1);
  sp_query_arena= new(dummy_table->s + 1) Query_arena();
  memset(&sp_mem_root, 0, sizeof(sp_mem_root));
}

LEX_CSTRING
Item_sp::func_name_cstring(THD *thd, bool is_package_function) const
{
  /* Calculate length to avoid reallocation of string for sure */
  size_t len= (((m_name->m_explicit_name ? m_name->m_db.length : 0) +
              m_name->m_name.length)*2 + //characters*quoting
             2 +                         // quotes for the function name
             2 +                         // quotes for the package name
             (m_name->m_explicit_name ?
              3 : 0) +                   // '`', '`' and '.' for the db
             1 +                         // '.' between package and function
             1 +                         // end of string
             ALIGN_SIZE(1));             // to avoid String reallocation
  String qname((char *)alloc_root(thd->mem_root, len), len,
               system_charset_info);

  qname.length(0);
  if (m_name->m_explicit_name)
  {
    append_identifier(thd, &qname, &m_name->m_db);
    qname.append('.');
  }
  if (is_package_function)
  {
    /*
      In case of a package function split `pkg.func` and print
      quoted `pkg` and `func` separately, so the entire result looks like:
         `db`.`pkg`.`func`
    */
    Database_qualified_name tmp= Database_qualified_name::split(m_name->m_name);
    DBUG_ASSERT(tmp.m_db.length);
    append_identifier(thd, &qname, &tmp.m_db);
    qname.append('.');
    append_identifier(thd, &qname, &tmp.m_name);
  }
  else
    append_identifier(thd, &qname, &m_name->m_name);
  return { qname.c_ptr_safe(), qname.length() };
}

void
Item_sp::cleanup()
{
  delete sp_result_field;
  sp_result_field= NULL;
  m_sp= NULL;
  delete func_ctx;
  func_ctx= NULL;
  free_root(&sp_mem_root, MYF(0));
  dummy_table->alias.free();
}

/**
  @brief Checks if requested access to function can be granted to user.
    If function isn't found yet, it searches function first.
    If function can't be found or user don't have requested access
    error is raised.

  @param thd thread handler

  @return Indication if the access was granted or not.
  @retval FALSE Access is granted.
  @retval TRUE Requested access can't be granted or function doesn't exists.

*/
bool
Item_sp::sp_check_access(THD *thd)
{
  DBUG_ENTER("Item_sp::sp_check_access");
  DBUG_ASSERT(m_sp);
  DBUG_RETURN(m_sp->check_execute_access(thd));
}

/**
  @brief Execute function & store value in field.

  @return Function returns error status.
  @retval FALSE on success.
  @retval TRUE if an error occurred.
*/
bool Item_sp::execute(THD *thd, bool *null_value, Item **args, uint arg_count)
{
  if (unlikely(execute_impl(thd, args, arg_count)))
  {
    *null_value= 1;
    process_error(thd);
    if (thd->killed)
      thd->send_kill_message();
    return true;
  }

  /* Check that the field (the value) is not NULL. */

  *null_value= sp_result_field->is_null();
  return (*null_value);
}

/**
   @brief Execute function and store the return value in the field.

   @note This function was intended to be the concrete implementation of
    the interface function execute. This was never realized.

   @return The error state.
   @retval FALSE on success
   @retval TRUE if an error occurred.
*/
bool
Item_sp::execute_impl(THD *thd, Item **args, uint arg_count)
{
  Sub_statement_state statement_state;
  Security_context *save_security_ctx= thd->security_ctx;
  enum enum_sp_data_access access=
    (m_sp->daccess() == SP_DEFAULT_ACCESS) ?
     SP_DEFAULT_ACCESS_MAPPING : m_sp->daccess();

  DBUG_ENTER("Item_sp::execute_impl");

  if (context && context->security_ctx)
  {
    /* Set view definer security context */
    thd->security_ctx= context->security_ctx;
  }

  if (unlikely(sp_check_access(thd)))
  {
    thd->security_ctx= save_security_ctx;
    DBUG_RETURN(TRUE);
  }

  /*
    Throw an error if a non-deterministic function is called while
    statement-based replication (SBR) is active.
  */

  if (unlikely(!m_sp->detistic() && !trust_function_creators &&
               (access == SP_CONTAINS_SQL || access == SP_MODIFIES_SQL_DATA) &&
               (mysql_bin_log.is_open() &&
                thd->variables.binlog_format == BINLOG_FORMAT_STMT)))
  {
    my_error(ER_BINLOG_UNSAFE_ROUTINE, MYF(0));
    thd->security_ctx= save_security_ctx;
    DBUG_RETURN(TRUE);
  }

  /*
    Disable the binlogging if this is not a SELECT statement. If this is a
    SELECT, leave binlogging on, so execute_function() code writes the
    function call into binlog.
  */
  thd->reset_sub_statement_state(&statement_state, SUB_STMT_FUNCTION);

  /*
     If this function is an aggregate function, we want to initialise the
     mem_root only once per group. For a regular stored function, we will
     initialise once for each call to execute_function.
  */
  m_sp->agg_type();
  DBUG_ASSERT(m_sp->agg_type() == GROUP_AGGREGATE ||
              (m_sp->agg_type() == NOT_AGGREGATE && !func_ctx));
  if (!func_ctx)
  {
    init_sql_alloc(key_memory_sp_head_call_root, &sp_mem_root,
                   MEM_ROOT_BLOCK_SIZE, 0, MYF(0));
    *sp_query_arena= Query_arena(&sp_mem_root,
                                 Query_arena::STMT_INITIALIZED_FOR_SP);
  }

  bool err_status= m_sp->execute_function(thd, args, arg_count,
                                          sp_result_field, &func_ctx,
                                          sp_query_arena);
  /*
     We free the function context when the function finished executing normally
     (quit_func == TRUE) or the function has exited with an error.
  */
  if (err_status || func_ctx->quit_func)
  {
    /* Free Items allocated during function execution. */
    delete func_ctx;
    func_ctx= NULL;
    sp_query_arena->free_items();
    free_root(&sp_mem_root, MYF(0));
    memset(&sp_mem_root, 0, sizeof(sp_mem_root));
  }
  thd->restore_sub_statement_state(&statement_state);

  thd->security_ctx= save_security_ctx;
  DBUG_RETURN(err_status);
}


/**
  @brief Initialize the result field by creating a temporary dummy table
    and assign it to a newly created field object. Meta data used to
    create the field is fetched from the sp_head belonging to the stored
    proceedure found in the stored procedure functon cache.

  @note This function should be called from fix_fields to init the result
    field. It is some what related to Item_field.

  @see Item_field

  @param thd A pointer to the session and thread context.

  @return Function return error status.
  @retval TRUE is returned on an error
  @retval FALSE is returned on success.
*/

bool
Item_sp::init_result_field(THD *thd, uint max_length, uint maybe_null,
                           bool *null_value, LEX_CSTRING *name)
{
  DBUG_ENTER("Item_sp::init_result_field");

  DBUG_ASSERT(m_sp != NULL);
  DBUG_ASSERT(sp_result_field == NULL);

  /*
     A Field needs to be attached to a Table.
     Below we "create" a dummy table by initializing
     the needed pointers.
   */
  dummy_table->alias.set("", 0, table_alias_charset);
  dummy_table->in_use= thd;
  dummy_table->copy_blobs= TRUE;
  dummy_table->s->table_cache_key= empty_clex_str;
  dummy_table->s->table_name= empty_clex_str;
  dummy_table->maybe_null= maybe_null;

  if (!(sp_result_field= m_sp->create_result_field(max_length, name,
                                                   dummy_table)))
   DBUG_RETURN(TRUE);

  if (sp_result_field->pack_length() > sizeof(result_buf))
  {
    void *tmp;
    if (!(tmp= thd->alloc(sp_result_field->pack_length())))
      DBUG_RETURN(TRUE);
    sp_result_field->move_field((uchar*) tmp);
  }
  else
    sp_result_field->move_field(result_buf);

  sp_result_field->null_ptr= (uchar *) null_value;
  sp_result_field->null_bit= 1;

  DBUG_RETURN(FALSE);
}

/**
  @brief
    Building clone for Item_ref
    
  @param thd        thread handle
  @param mem_root   part of the memory for the clone   

  @details
    This method gets copy of the current item and also 
    builds clone for its reference. 
      
   @retval
     clone of the item
     0 if an error occurred
*/ 

Item* Item_ref::build_clone(THD *thd)
{
  Item_ref *copy= (Item_ref *) get_copy(thd);
  if (unlikely(!copy) ||
      unlikely(!(copy->ref= (Item**) alloc_root(thd->mem_root,
                                                sizeof(Item*)))) ||
      unlikely(!(*copy->ref= (* ref)->build_clone(thd))))
    return 0;
  return copy;
}


/**********************************************/

Item_field::Item_field(THD *thd, Field *f)
  :Item_ident(thd, 0, null_clex_str,
              Lex_cstring_strlen(*f->table_name), f->field_name),
   item_equal(0),
   have_privileges(NO_ACL), any_privileges(0)
{
  set_field(f);
  /*
    field_name and table_name should not point to garbage
    if this item is to be reused
  */
  orig_table_name= table_name;
  orig_field_name= field_name;
  with_flags|= item_with_t::FIELD;
}


/**
  Constructor used inside setup_wild().

  Ensures that field, table, and database names will live as long as
  Item_field (this is important in prepared statements).
*/

Item_field::Item_field(THD *thd, Name_resolution_context *context_arg,
                       Field *f)
  :Item_ident(thd, context_arg, f->table->s->db,
              Lex_cstring_strlen(*f->table_name), f->field_name),
   item_equal(0), have_privileges(NO_ACL), any_privileges(0)
{
  /*
    We always need to provide Item_field with a fully qualified field
    name to avoid ambiguity when executing prepared statements like
    SELECT * from d1.t1, d2.t1; (assuming d1.t1 and d2.t1 have columns
    with same names).
    This is because prepared statements never deal with wildcards in
    select list ('*') and always fix fields using fully specified path
    (i.e. db.table.column).
    No check for OOM: if db_name is NULL, we'll just get
    "Field not found" error.
    We need to copy db_name, table_name and field_name because they must
    be allocated in the statement memory, not in table memory (the table
    structure can go away and pop up again between subsequent executions
    of a prepared statement or after the close_tables_for_reopen() call
    in mysql_multi_update_prepare() or due to wildcard expansion in stored
    procedures).
  */
  {
    if (db_name.str)
      orig_db_name= thd->strmake_lex_cstring(db_name);
    if (table_name.str)
      orig_table_name= thd->strmake_lex_cstring(table_name);
    if (field_name.str)
      orig_field_name= thd->strmake_lex_cstring(field_name);
    /*
      We don't restore 'name' in cleanup because it's not changed
      during execution. Still we need it to point to persistent
      memory if this item is to be reused.
    */
    name= orig_field_name;
  }
  set_field(f);
  with_flags|= item_with_t::FIELD;
}


Item_field::Item_field(THD *thd, Name_resolution_context *context_arg,
                       const LEX_CSTRING &db_arg,
                       const LEX_CSTRING &table_name_arg,
                       const LEX_CSTRING &field_name_arg)
  :Item_ident(thd, context_arg, db_arg, table_name_arg, field_name_arg),
   field(0), item_equal(0),
   have_privileges(NO_ACL), any_privileges(0)
{
  SELECT_LEX *select= thd->lex->current_select;
  collation.set(DERIVATION_IMPLICIT);
  if (select && select->parsing_place != IN_HAVING)
      select->select_n_where_fields++;
  with_flags|= item_with_t::FIELD;
}

/**
  Constructor need to process subselect with temporary tables (see Item)
*/

Item_field::Item_field(THD *thd, Item_field *item)
  :Item_ident(thd, item),
   field(item->field),
   item_equal(item->item_equal),
   have_privileges(item->have_privileges),
   any_privileges(item->any_privileges)
{
  collation.set(DERIVATION_IMPLICIT);
  with_flags|= item_with_t::FIELD;
}


void Item_field::set_field(Field *field_par)
{
  field=result_field=field_par;			// for easy coding with fields
  set_maybe_null(field->maybe_null());
  Type_std_attributes::set(field_par->type_std_attributes()); 
  table_name= Lex_cstring_strlen(*field_par->table_name);
  field_name= field_par->field_name;
  db_name= field_par->table->s->db;
  alias_name_used= field_par->table->alias_name_used;

  base_flags|= item_base_t::FIXED;
  if (field->table->s->tmp_table == SYSTEM_TMP_TABLE)
    any_privileges= 0;

  if (field->table->s->tmp_table == SYSTEM_TMP_TABLE ||
      field->table->s->tmp_table == INTERNAL_TMP_TABLE)
    set_refers_to_temp_table(true);
}


/**
  Reset this item to point to a field from the new temporary table.
  This is used when we create a new temporary table for each execution
  of prepared statement.
*/

void Item_field::reset_field(Field *f)
{
  set_field(f);
  /* 'name' is pointing at field->field_name of old field */
  name= f->field_name;
}


void Item_field::load_data_print_for_log_event(THD *thd, String *to) const
{
  append_identifier(thd, to, name.str, name.length);
}


bool Item_field::load_data_set_no_data(THD *thd, const Load_data_param *param)
{
  if (field->load_data_set_no_data(thd, param->is_fixed_length()))
    return true;
  /*
    TODO: We probably should not throw warning for each field.
    But how about intention to always have the same number
    of warnings in THD::cuted_fields (and get rid of cuted_fields
    in the end ?)
  */
  thd->cuted_fields++;
  push_warning_printf(thd, Sql_condition::WARN_LEVEL_WARN,
                      ER_WARN_TOO_FEW_RECORDS,
                      ER_THD(thd, ER_WARN_TOO_FEW_RECORDS),
                      thd->get_stmt_da()->current_row_for_warning());
  return false;
}


bool Item_field::enumerate_field_refs_processor(void *arg)
{
  Field_enumerator *fe= (Field_enumerator*)arg;
  fe->visit_field(this);
  return FALSE;
}

bool Item_field::update_table_bitmaps_processor(void *arg)
{
  update_table_bitmaps();
  return FALSE;
}

static inline void set_field_to_new_field(Field **field, Field **new_field)
{
  if (*field && (*field)->table == new_field[0]->table)
  {
    Field *newf= new_field[(*field)->field_index];
    if ((*field)->ptr == newf->ptr)
      *field= newf;
  }
}

bool Item_field::switch_to_nullable_fields_processor(void *arg)
{
  Field **new_fields= (Field **)arg;
  set_field_to_new_field(&field, new_fields);
  set_field_to_new_field(&result_field, new_fields);
  set_maybe_null(field && field->maybe_null());
  return 0;
}

LEX_CSTRING Item_ident::full_name_cstring() const
{
  char *tmp;
  size_t length;
  if (!table_name.str || !field_name.str)
  {
    if (field_name.str)
      return field_name;
    if (name.str)
      return name;
    return { STRING_WITH_LEN("tmp_field") };
  }
  if (db_name.str && db_name.str[0])
  {
    THD *thd= current_thd;
    tmp=(char*) thd->alloc((uint) db_name.length+ (uint) table_name.length +
			   (uint) field_name.length+3);
    length= (strxmov(tmp,db_name.str,".",table_name.str,".",field_name.str,
                     NullS) - tmp);
  }
  else
  {
    if (!table_name.str[0])
      return field_name;

    THD *thd= current_thd;
    tmp= (char*) thd->alloc((uint) table_name.length +
                            field_name.length + 2);
    length= (strxmov(tmp, table_name.str, ".", field_name.str, NullS) - tmp);
  }
  return {tmp, length};
}

void Item_ident::print(String *str, enum_query_type query_type)
{
  THD *thd= current_thd;
  char d_name_buff[MAX_ALIAS_NAME], t_name_buff[MAX_ALIAS_NAME];
  LEX_CSTRING d_name= db_name;
  LEX_CSTRING t_name= table_name;
  bool use_table_name= table_name.str && table_name.str[0];
  bool use_db_name= use_table_name && db_name.str && db_name.str[0] &&
                    !alias_name_used;

  if (use_db_name && (query_type & QT_ITEM_IDENT_SKIP_DB_NAMES))
    use_db_name= !thd->db.str || strcmp(thd->db.str, db_name.str);

  if (use_db_name)
    use_db_name= !(cached_table && cached_table->belong_to_view &&
                   cached_table->belong_to_view->compact_view_format);

  if (use_table_name && (query_type & QT_ITEM_IDENT_SKIP_TABLE_NAMES))
  {
    /*
      Don't print the table name if it's the only table in the context
      XXX technically, that's a sufficient, but too strong condition
    */
    if (!context)
      use_db_name= use_table_name= false;
    else if (context->outer_context)
      use_table_name= true;
    else if (context->last_name_resolution_table == context->first_name_resolution_table)
      use_db_name= use_table_name= false;
    else if (!context->last_name_resolution_table &&
             !context->first_name_resolution_table->next_name_resolution_table)
      use_db_name= use_table_name= false;
  }

  if ((query_type & QT_ITEM_IDENT_DISABLE_DB_TABLE_NAMES))
  {
    // Don't print db or table name irrespective of any other settings.
    use_db_name= use_table_name= false;
  }

  if (!field_name.str || !field_name.str[0])
  {
    append_identifier(thd, str, STRING_WITH_LEN("tmp_field"));
    return;
  }

  if (lower_case_table_names== 1 ||
      (lower_case_table_names == 2 && !alias_name_used))
  {
    if (use_table_name)
    {
      strmov(t_name_buff, table_name.str);
      my_casedn_str(files_charset_info, t_name_buff);
      t_name= Lex_cstring_strlen(t_name_buff);
    }
    if (use_db_name)
    {
      strmov(d_name_buff, db_name.str);
      my_casedn_str(files_charset_info, d_name_buff);
      d_name= Lex_cstring_strlen(d_name_buff);
    }
  }

  if (use_db_name)
  {
    append_identifier(thd, str, d_name.str, (uint) d_name.length);
    str->append('.');
    DBUG_ASSERT(use_table_name);
  }
  if (use_table_name)
  {
    append_identifier(thd, str, t_name.str, (uint) t_name.length);
    str->append('.');
  }
  append_identifier(thd, str, &field_name);
}

/* ARGSUSED */
String *Item_field::val_str(String *str)
{
  DBUG_ASSERT(fixed());
  if ((null_value=field->is_null()))
    return 0;
  str->set_charset(str_value.charset());
  return field->val_str(str,&str_value);
}


double Item_field::val_real()
{
  DBUG_ASSERT(fixed());
  if ((null_value=field->is_null()))
    return 0.0;
  return field->val_real();
}


longlong Item_field::val_int()
{
  DBUG_ASSERT(fixed());
  if ((null_value=field->is_null()))
    return 0;
  return field->val_int();
}


my_decimal *Item_field::val_decimal(my_decimal *decimal_value)
{
  if ((null_value= field->is_null()))
    return 0;
  return field->val_decimal(decimal_value);
}


String *Item_field::str_result(String *str)
{
  if ((null_value=result_field->is_null()))
    return 0;
  str->set_charset(str_value.charset());
  return result_field->val_str(str,&str_value);
}

bool Item_field::get_date(THD *thd, MYSQL_TIME *ltime,date_mode_t fuzzydate)
{
  if ((null_value=field->is_null()) || field->get_date(ltime,fuzzydate))
  {
    bzero((char*) ltime,sizeof(*ltime));
    return 1;
  }
  return 0;
}

bool Item_field::get_date_result(THD *thd, MYSQL_TIME *ltime, date_mode_t fuzzydate)
{
  if ((null_value= result_field->is_null()) ||
      result_field->get_date(ltime, fuzzydate))
  {
    bzero((char*) ltime,sizeof(*ltime));
    return true;
  }
  return false;
}


bool Item_field::val_native(THD *thd, Native *to)
{
  return val_native_from_field(field, to);
}


bool Item_field::val_native_result(THD *thd, Native *to)
{
  return val_native_from_field(result_field, to);
}


longlong Item_field::val_datetime_packed(THD *thd)
{
  DBUG_ASSERT(fixed());
  if ((null_value= field->is_null()))
    return 0;
  return field->val_datetime_packed(thd);
}


longlong Item_field::val_time_packed(THD *thd)
{
  DBUG_ASSERT(fixed());
  if ((null_value= field->is_null()))
    return 0;
  return field->val_time_packed(thd);
}


void Item_field::save_result(Field *to)
{
  save_field_in_field(result_field, &null_value, to, TRUE);
}


double Item_field::val_result()
{
  if ((null_value=result_field->is_null()))
    return 0.0;
  return result_field->val_real();
}

longlong Item_field::val_int_result()
{
  if ((null_value=result_field->is_null()))
    return 0;
  return result_field->val_int();
}


my_decimal *Item_field::val_decimal_result(my_decimal *decimal_value)
{
  if ((null_value= result_field->is_null()))
    return 0;
  return result_field->val_decimal(decimal_value);
}


bool Item_field::val_bool_result()
{
  if ((null_value= result_field->is_null()))
    return false;
  return result_field->val_bool();
}


bool Item_field::is_null_result()
{
  return (null_value=result_field->is_null());
}


bool Item_field::eq(const Item *item, bool binary_cmp) const
{
  Item *real_item2= ((Item *) item)->real_item();
  if (real_item2->type() != FIELD_ITEM)
    return 0;
  
  Item_field *item_field= (Item_field*) real_item2;
  if (item_field->field && field)
    return item_field->field == field;
  /*
    We may come here when we are trying to find a function in a GROUP BY
    clause from the select list.
    In this case the '100 % correct' way to do this would be to first
    run fix_fields() on the GROUP BY item and then retry this function, but
    I think it's better to relax the checking a bit as we will in
    most cases do the correct thing by just checking the field name.
    (In cases where we would choose wrong we would have to generate a
    ER_NON_UNIQ_ERROR).
  */
  return (!lex_string_cmp(system_charset_info, &item_field->name,
                          &field_name) &&
	  (!item_field->table_name.str || !table_name.str ||
	   (!my_strcasecmp(table_alias_charset, item_field->table_name.str,
			   table_name.str) &&
	    (!item_field->db_name.str || !db_name.str ||
	     (item_field->db_name.str && !strcmp(item_field->db_name.str,
                                                 db_name.str))))));
}


table_map Item_field::used_tables() const
{
  if (field->table->const_table)
    return 0;					// const item
  return (get_depended_from() ? OUTER_REF_TABLE_BIT : field->table->map);
}

table_map Item_field::all_used_tables() const
{
  return (get_depended_from() ? OUTER_REF_TABLE_BIT : field->table->map);
}


bool Item_field::find_not_null_fields(table_map allowed)
{
  if (field->table->const_table)
    return false;
  if (!get_depended_from() && field->real_maybe_null())
    bitmap_set_bit(&field->table->tmp_set, field->field_index);
  return false;
}


/*
  @Note  thd->fatal_error can be set in case of OOM
*/

void Item_field::fix_after_pullout(st_select_lex *new_parent, Item **ref,
                                   bool merge)
{
  if (new_parent == get_depended_from())
    depended_from= NULL;
  if (context)
  {
    bool need_change= false;
    /*
      Suppose there are nested selects:

       select_id=1
         select_id=2
           select_id=3  <----+
             select_id=4    -+
               select_id=5 --+

      Suppose, pullout operation has moved anything that had select_id=4 or 5
      in to select_id=3.

      If this Item_field had a name resolution context pointing into select_lex
      with id=4 or id=5, it needs a new name resolution context.

      However, it could also be that this object is a part of outer reference:
      Item_ref(Item_field(field in select with select_id=1))).
      - The Item_ref object has a context with select_id=5, and so needs a new
        name resolution context.
      - The Item_field object has a context with select_id=1, and doesn't need
        a new name resolution context.

      So, the following loop walks from Item_field's current context upwards.
      If we find that the select we've been pulled out to is up there, we
      create the new name resolution context. Otherwise, we don't.
    */
    for (Name_resolution_context *ct= context; ct; ct= ct->outer_context)
    {
      if (new_parent == ct->select_lex)
      {
        need_change= true;
        break;
      }
    }
    if (!need_change)
      return;

    if (!merge)
    {
      /*
        It is transformation without merge.
        This field was "outer" for the inner SELECT where it was taken and
        moved up.
        "Outer" fields uses normal SELECT_LEX context of upper SELECTs for
        name resolution, so we can switch everything to it safely.
      */
      this->context= &new_parent->context;
      return;
    }

    Name_resolution_context *ctx= new Name_resolution_context();
    if (!ctx)
      return;                                   // Fatal error set
    if (context->select_lex == new_parent)
    {
      /*
        This field was pushed in then pulled out
        (for example left part of IN)
      */
      ctx->outer_context= context->outer_context;
    }
    else if (context->outer_context)
    {
      /* just pull to the upper context */
      ctx->outer_context= context->outer_context->outer_context;
    }
    else
    {
      /* No upper context (merging Derived/VIEW where context chain ends) */
      ctx->outer_context= NULL;
    }
    ctx->table_list= context->first_name_resolution_table;
    ctx->select_lex= new_parent;
    if (context->select_lex == NULL)
      ctx->select_lex= NULL;
    ctx->first_name_resolution_table= context->first_name_resolution_table;
    ctx->last_name_resolution_table=  context->last_name_resolution_table;
    ctx->error_processor=             context->error_processor;
    ctx->error_processor_data=        context->error_processor_data;
    ctx->resolve_in_select_list=      context->resolve_in_select_list;
    ctx->security_ctx=                context->security_ctx;
    this->context=ctx;
  }
}


Item *Item_field::get_tmp_table_item(THD *thd)
{
  Item_field *new_item= new (thd->mem_root) Item_field(thd, this);
  if (new_item)
  {
    new_item->field= new_item->result_field;
    new_item->set_refers_to_temp_table(true);
  }
  return new_item;
}

longlong Item_field::val_int_endpoint(bool left_endp, bool *incl_endp)
{
  longlong res= val_int();
  return null_value? LONGLONG_MIN : res;
}

void Item_field::set_refers_to_temp_table(bool value)
{
  refers_to_temp_table= value;
}


bool Item_basic_value::eq(const Item *item, bool binary_cmp) const
{
  const Item_const *c0, *c1;
  const Type_handler *h0, *h1;
  /*
    - Test get_item_const() for NULL filters out Item_param
      bound in a way that needs a data type conversion
      (e.g. non-integer value in a LIMIT clause).
      Item_param::get_item_const() return NULL in such cases.
    - Test for type_handler_for_comparison() equality makes sure
      that values of different data type groups do not get detected
      as equal (e.g. numbers vs strings, time vs datetime).
    - Test for cast_to_int_type_handler() equality distinguishes
      values with dual properties. For example, VARCHAR 'abc' and hex
      hybrid 0x616263 are equal in string context, but they are not equal
      if the hybrid appears in integer context (it behaves as integer then).
      Here we have no full information about the context, so treat them
      as not equal.
      QQ: We could pass Value_source::Context here instead of
      "bool binary_cmp", to make substitution more delicate.
      See Field::get_equal_const_item().
  */
  bool res= (c0= get_item_const()) &&
            (c1= item->get_item_const()) &&
            (h0= type_handler())->type_handler_for_comparison() ==
            (h1= item->type_handler())->type_handler_for_comparison() &&
            h0->cast_to_int_type_handler()->type_handler_for_comparison() ==
            h1->cast_to_int_type_handler()->type_handler_for_comparison();
  if (res)
  {
    switch (c0->const_is_null() + c1->const_is_null()) {
    case 2:       // Two NULLs
      res= true;
      break;
    case 1:       // NULL and non-NULL
      res= false;
      break;
    case 0:       // Two non-NULLs
      res= h0->Item_const_eq(c0, c1, binary_cmp);
    }
  }
  DBUG_EXECUTE_IF("Item_basic_value",
                  push_warning_printf(current_thd,
                  Sql_condition::WARN_LEVEL_NOTE,
                  ER_UNKNOWN_ERROR, "%seq=%d a=%s b=%s",
                  binary_cmp ? "bin_" : "", (int) res,
                  DbugStringItemTypeValue(current_thd, this).c_ptr(),
                  DbugStringItemTypeValue(current_thd, item).c_ptr()
                  ););
  return res;
}


/**
  Create an item from a string we KNOW points to a valid longlong
  end \\0 terminated number string.
  This is always 'signed'. Unsigned values are created with Item_uint()
*/

Item_int::Item_int(THD *thd, const char *str_arg, size_t length):
  Item_num(thd)
{
  char *end_ptr= (char*) str_arg + length;
  int error;
  value= my_strtoll10(str_arg, &end_ptr, &error);
  max_length= (uint) (end_ptr - str_arg);
  name.str= str_arg;
  /*
    We can't trust max_length as in show_routine_code we are using "Pos" as
    the field name.
  */
  name.length= !str_arg[max_length] ? max_length : strlen(str_arg);
}


my_decimal *Item_int::val_decimal(my_decimal *decimal_value)
{
  int2my_decimal(E_DEC_FATAL_ERROR, value, unsigned_flag, decimal_value);
  return decimal_value;
}

String *Item_int::val_str(String *str)
{
  str->set_int(value, unsigned_flag, collation.collation);
  return str;
}


void Item_int::print(String *str, enum_query_type query_type)
{
  StringBuffer<LONGLONG_BUFFER_SIZE+1> buf;
  // my_charset_bin is good enough for numbers
  buf.set_int(value, unsigned_flag, &my_charset_bin);
  str->append(buf);
}


Item *Item_bool::neg_transformer(THD *thd)
{
  value= !value;
  name= null_clex_str;
  return this;
}


Item_uint::Item_uint(THD *thd, const char *str_arg, size_t length):
  Item_int(thd, str_arg, length)
{
  unsigned_flag= 1;
}


Item_uint::Item_uint(THD *thd, const char *str_arg, longlong i, uint length):
  Item_int(thd, str_arg, i, length)
{
  unsigned_flag= 1;
}


Item_decimal::Item_decimal(THD *thd, const char *str_arg, size_t length,
                           CHARSET_INFO *charset):
  Item_num(thd)
{
  str2my_decimal(E_DEC_FATAL_ERROR, str_arg, length, charset, &decimal_value);
  name.str= str_arg;
  name.length= safe_strlen(str_arg);
  decimals= (uint8) decimal_value.frac;
  max_length= my_decimal_precision_to_length_no_truncation(decimal_value.intg +
                                                           decimals,
                                                           decimals,
                                                           unsigned_flag);
}

Item_decimal::Item_decimal(THD *thd, longlong val, bool unsig):
  Item_num(thd)
{
  int2my_decimal(E_DEC_FATAL_ERROR, val, unsig, &decimal_value);
  decimals= (uint8) decimal_value.frac;
  max_length= my_decimal_precision_to_length_no_truncation(decimal_value.intg +
                                                           decimals,
                                                           decimals,
                                                           unsigned_flag);
}


Item_decimal::Item_decimal(THD *thd, double val, int precision, int scale):
  Item_num(thd)
{
  double2my_decimal(E_DEC_FATAL_ERROR, val, &decimal_value);
  decimals= (uint8) decimal_value.frac;
  max_length= my_decimal_precision_to_length_no_truncation(decimal_value.intg +
                                                           decimals,
                                                           decimals,
                                                           unsigned_flag);
}


Item_decimal::Item_decimal(THD *thd, const char *str, const my_decimal *val_arg,
                           uint decimal_par, uint length):
  Item_num(thd)
{
  my_decimal2decimal(val_arg, &decimal_value);
  name.str= str;
  name.length= safe_strlen(str);
  decimals= (uint8) decimal_par;
  max_length= length;
}


Item_decimal::Item_decimal(THD *thd, const my_decimal *value_par):
  Item_num(thd)
{
  my_decimal2decimal(value_par, &decimal_value);
  decimals= (uint8) decimal_value.frac;
  max_length= my_decimal_precision_to_length_no_truncation(decimal_value.intg +
                                                           decimals,
                                                           decimals,
                                                           unsigned_flag);
}


Item_decimal::Item_decimal(THD *thd, const uchar *bin, int precision, int scale):
  Item_num(thd),
  decimal_value(bin, precision, scale)
{
  decimals= (uint8) decimal_value.frac;
  max_length= my_decimal_precision_to_length_no_truncation(precision, decimals,
                                                           unsigned_flag);
}


void Item_decimal::set_decimal_value(my_decimal *value_par)
{
  my_decimal2decimal(value_par, &decimal_value);
  decimals= (uint8) decimal_value.frac;
  unsigned_flag= !decimal_value.sign();
  max_length= my_decimal_precision_to_length_no_truncation(decimal_value.intg +
                                                           decimals,
                                                           decimals,
                                                           unsigned_flag);
}


Item *Item_decimal::clone_item(THD *thd)
{
  return new (thd->mem_root) Item_decimal(thd, name.str, &decimal_value, decimals,
                                         max_length);
}


String *Item_float::val_str(String *str)
{
  str->set_real(value, decimals, &my_charset_numeric);
  return str;
}


my_decimal *Item_float::val_decimal(my_decimal *decimal_value)
{
  double2my_decimal(E_DEC_FATAL_ERROR, value, decimal_value);
  return (decimal_value);
}


Item *Item_float::clone_item(THD *thd)
{
  return new (thd->mem_root) Item_float(thd, name.str, value, decimals,
                                       max_length);
}


void Item_string::print(String *str, enum_query_type query_type)
{
  const bool print_introducer=
    !(query_type & QT_WITHOUT_INTRODUCERS) && is_cs_specified();
  if (print_introducer)
  {
    str->append('_');
    str->append(collation.collation->cs_name);
  }

  str->append('\'');

  if (query_type & QT_TO_SYSTEM_CHARSET)
  {
    if (print_introducer)
    {
      /*
        Because we wrote an introducer, we must print str_value in its
        charset, and the resulting bytes must not be changed until they
        reach the end client.
        But the caller is asking for system_charset_info, and may later
        convert into character_set_results. That means two conversions: we
        must ensure that they don't change our printed bytes.
        So we print str_value in the least common denominator of the three
        charsets involved: ASCII. Non-ASCII characters are printed as \xFF
        sequences (which is ASCII too). This way, our bytes will not be
        changed.
      */
      ErrConvString tmp(str_value.ptr(), str_value.length(), &my_charset_bin);
      str->append(tmp.lex_cstring());
    }
    else
    {
      str_value.print(str, system_charset_info);
    }
  }
  else
  {
    /*
      We're restoring a parse-able statement from an Item tree.
      Make sure to revert character set conversions that previously
      happened in the parser when Item_string was created.
    */
    if (print_introducer)
    {
      /*
        Print the string as is, without conversion:
        Strings with introducers are not converted in the parser.
      */
      str_value.print(str);
    }
    else
    {
      /*
        Print the string with conversion.
        Strings without introducers are converted in the parser,
        from character_set_client to character_set_connection.

        When restoring a CREATE VIEW statement,
        - str_value.charsets() contains parse time character_set_connection
        - str->charset() contains parse time character_set_client
        So we convert the string back from parse-time character_set_connection
        to parse time character_set_client.

        In some cases, e.g. SHOW PROCEDURE CODE, it's also possible
        that str->charset() is "utf8mb3" instead of parse time
        character_set_client. In these cases we convert
        here from the parse-time character_set_connection to utf8mb3.

        QQ: perhaps the code behind SHOW PROCEDURE CODE should
        also request the result in the parse-time character_set_client
        (like the code restoring CREATE VIEW statements does),
        rather than in utf8mb3:
        - utf8mb3 does not work well with non-BMP characters (e.g. emoji).
        - Simply changing utf8mb3 to utf8mb4 will not fully help:
          some character sets have unassigned characters,
          they get lost during during cs->utf8mb4->cs round trip.
      */
      str_value.print_with_conversion(str, str->charset());
    }
  }

  str->append('\'');
}


double Item_string::val_real()
{
  return double_from_string_with_check(&str_value);
}


/**
  @todo
  Give error if we wanted a signed integer and we got an unsigned one
*/
longlong Item_string::val_int()
{
  return longlong_from_string_with_check(&str_value);
}


my_decimal *Item_string::val_decimal(my_decimal *decimal_value)
{
  return val_decimal_from_string(decimal_value);
}


double Item_null::val_real()
{
  null_value=1;
  return 0.0;
}
longlong Item_null::val_int()
{
  null_value=1;
  return 0;
}
/* ARGSUSED */
String *Item_null::val_str(String *str)
{
  null_value=1;
  return 0;
}

my_decimal *Item_null::val_decimal(my_decimal *decimal_value)
{
  return 0;
}


longlong Item_null::val_datetime_packed(THD *)
{
  null_value= true;
  return 0;
}


longlong Item_null::val_time_packed(THD *)
{
  null_value= true;
  return 0;
}


bool Item_null::get_date(THD *thd, MYSQL_TIME *ltime, date_mode_t fuzzydate)
{
  set_zero_time(ltime, MYSQL_TIMESTAMP_NONE);
  return (null_value= true);
}


Item *Item_null::safe_charset_converter(THD *thd, CHARSET_INFO *tocs)
{
  return this;
}

Item *Item_null::clone_item(THD *thd)
{
  return new (thd->mem_root) Item_null(thd, name.str);
}


Item_basic_constant *
Item_null::make_string_literal_concat(THD *thd, const LEX_CSTRING *str)
{
  DBUG_ASSERT(thd->variables.sql_mode & MODE_EMPTY_STRING_IS_NULL);
  if (str->length)
  {
    CHARSET_INFO *cs= thd->variables.collation_connection;
    my_repertoire_t repertoire= my_string_repertoire(cs, str->str, str->length);
    return new (thd->mem_root) Item_string(thd,
                                           str->str, (uint) str->length, cs,
                                           DERIVATION_COERCIBLE, repertoire);
  }
  return this;
}


/*********************** Item_param related ******************************/

Item_param::Item_param(THD *thd, const LEX_CSTRING *name_arg,
                       uint pos_in_query_arg, uint len_in_query_arg):
  Item_basic_value(thd),
  Rewritable_query_parameter(pos_in_query_arg, len_in_query_arg),
  /*
    Set handler to type_handler_null. Its data type test methods such as:
    - is_scalar_type()
    - can_return_int()
    - can_return_real(),
    - is_general_purpose_string_type()
    all return "true". This is needed to avoid any "illegal parameter type"
    errors in Item::check_type_xxx() at PS prepare time.
  */
  Type_handler_hybrid_field_type(&type_handler_null),
  state(NO_VALUE),
  m_empty_string_is_null(false),
  indicator(STMT_INDICATOR_NONE),
  m_out_param_info(NULL),
  /*
    Set m_is_settable_routine_parameter to "true" by default.
    This is needed for client-server protocol,
    whose parameters are always settable.
    For dynamic SQL, settability depends on the type of Item passed
    as an actual parameter. See Item_param::set_from_item().
  */
  m_is_settable_routine_parameter(true),
  m_clones(thd->mem_root)
{
  name= *name_arg;
  /*
    Since we can't say whenever this item can be NULL or cannot be NULL
    before mysql_stmt_execute(), so we assuming that it can be NULL until
    value is set.
  */
  set_maybe_null();
}


/* Add reference to Item_param used in a copy of CTE to its master as a clone */

bool Item_param::add_as_clone(THD *thd)
{
  LEX *lex= thd->lex;
  my_ptrdiff_t master_pos= pos_in_query + lex->clone_spec_offset;
  List_iterator_fast<Item_param> it(lex->param_list);
  Item_param *master_param;
  while ((master_param = it++))
  {
    if (master_pos == master_param->pos_in_query)
      return master_param->register_clone(this);
  }
  DBUG_ASSERT(false);
  return false;
}


/* Update all clones of Item_param to sync their values with the item's value */

void Item_param::sync_clones()
{
  Item_param **c_ptr= m_clones.begin();
  Item_param **end= m_clones.end();
  for ( ; c_ptr < end; c_ptr++)
  {
    Item_param *c= *c_ptr;
    /* Scalar-type members: */
    c->copy_flags(this, item_base_t::MAYBE_NULL);
    c->null_value= null_value;
    c->Type_std_attributes::operator=(*this);
    c->Type_handler_hybrid_field_type::operator=(*this);

    c->state= state;
    c->m_empty_string_is_null= m_empty_string_is_null;

    c->value.PValue_simple::operator=(value);
    c->value.Type_handler_hybrid_field_type::operator=(value);
    type_handler()->Item_param_setup_conversion(current_thd, c);

    /* Class-type members: */
    c->value.m_decimal= value.m_decimal;
    /*
      Note that String's assignment op properly sets m_is_alloced to 'false',
      which is correct here: c->str_value doesn't own anything.
    */
    c->value.m_string= value.m_string;
    c->value.m_string_ptr= value.m_string_ptr;
  }
}


void Item_param::set_null()
{
  DBUG_ENTER("Item_param::set_null");
  /*
    These are cleared after each execution by reset() method or by setting
    other value.
  */
  null_value= 1;
  /* 
    Because of NULL and string values we need to set max_length for each new
    placeholder value: user can submit NULL for any placeholder type, and 
    string length can be different in each execution.
  */
  max_length= 0;
  decimals= 0;
  state= NULL_VALUE;
  DBUG_VOID_RETURN;
}

void Item_param::set_int(longlong i, uint32 max_length_arg)
{
  DBUG_ENTER("Item_param::set_int");
  DBUG_ASSERT(value.type_handler()->cmp_type() == INT_RESULT);
  value.integer= (longlong) i;
  state= SHORT_DATA_VALUE;
  collation= DTCollation_numeric();
  max_length= max_length_arg;
  decimals= 0;
  base_flags&= ~item_base_t::MAYBE_NULL;
  null_value= 0;
  DBUG_VOID_RETURN;
}

void Item_param::set_double(double d)
{
  DBUG_ENTER("Item_param::set_double");
  DBUG_ASSERT(value.type_handler()->cmp_type() == REAL_RESULT);
  value.real= d;
  state= SHORT_DATA_VALUE;
  collation= DTCollation_numeric();
  max_length= DBL_DIG + 8;
  decimals= NOT_FIXED_DEC;
  base_flags&= ~item_base_t::MAYBE_NULL;
  null_value= 0;
  DBUG_VOID_RETURN;
}


/**
  Set decimal parameter value from string.

  @param str      character string
  @param length   string length

  @note
    As we use character strings to send decimal values in
    binary protocol, we use str2my_decimal to convert it to
    internal decimal value.
*/

void Item_param::set_decimal(const char *str, ulong length)
{
  char *end;
  DBUG_ENTER("Item_param::set_decimal");
  DBUG_ASSERT(value.type_handler()->cmp_type() == DECIMAL_RESULT);

  end= (char*) str+length;
  str2my_decimal(E_DEC_FATAL_ERROR, str, &value.m_decimal, &end);
  state= SHORT_DATA_VALUE;
  decimals= value.m_decimal.frac;
  collation= DTCollation_numeric();
  max_length=
    my_decimal_precision_to_length_no_truncation(value.m_decimal.precision(),
                                                 decimals, unsigned_flag);
  base_flags&= ~item_base_t::MAYBE_NULL;
  null_value= 0;
  DBUG_VOID_RETURN;
}

void Item_param::set_decimal(const my_decimal *dv, bool unsigned_arg)
{
  DBUG_ASSERT(value.type_handler()->cmp_type() == DECIMAL_RESULT);
  state= SHORT_DATA_VALUE;

  my_decimal2decimal(dv, &value.m_decimal);

  decimals= (uint8) value.m_decimal.frac;
  collation= DTCollation_numeric();
  unsigned_flag= unsigned_arg;
  max_length= my_decimal_precision_to_length(value.m_decimal.intg + decimals,
                                             decimals, unsigned_flag);
  base_flags&= ~item_base_t::MAYBE_NULL;
  null_value= 0;
}


void Item_param::fix_temporal(uint32 max_length_arg, uint decimals_arg)
{
  state= SHORT_DATA_VALUE;
  collation= DTCollation_numeric();
  max_length= max_length_arg;
  decimals= decimals_arg;
  base_flags&= ~item_base_t::MAYBE_NULL;
  null_value= 0;
}


void Item_param::set_time(const MYSQL_TIME *tm,
                          uint32 max_length_arg, uint decimals_arg)
{
  DBUG_ASSERT(value.type_handler()->cmp_type() == TIME_RESULT);
  value.time= *tm;
  base_flags&= ~item_base_t::MAYBE_NULL;
  null_value= 0;
  fix_temporal(max_length_arg, decimals_arg);
}


/**
  Set parameter value from MYSQL_TIME value.

  @param tm              datetime value to set (time_type is ignored)
  @param type            type of datetime value
  @param max_length_arg  max length of datetime value as string

  @note
    If we value to be stored is not normalized, zero value will be stored
    instead and proper warning will be produced. This function relies on
    the fact that even wrong value sent over binary protocol fits into
    MAX_DATE_STRING_REP_LENGTH buffer.
*/
void Item_param::set_time(MYSQL_TIME *tm, timestamp_type time_type,
                          uint32 max_length_arg)
{ 
  DBUG_ENTER("Item_param::set_time");
  DBUG_ASSERT(value.type_handler()->cmp_type() == TIME_RESULT);

  value.time= *tm;
  value.time.time_type= time_type;

  if (check_datetime_range(&value.time))
  {
    ErrConvTime str(&value.time);
    make_truncated_value_warning(current_thd, Sql_condition::WARN_LEVEL_WARN,
                                 &str, time_type, NULL, NULL, NULL);
    set_zero_time(&value.time, time_type);
  }
  base_flags&= ~item_base_t::MAYBE_NULL;
  null_value= 0;
  fix_temporal(max_length_arg,
               tm->second_part > 0 ? TIME_SECOND_PART_DIGITS : 0);
  DBUG_VOID_RETURN;
}


bool Item_param::set_str(const char *str, ulong length,
                         CHARSET_INFO *fromcs, CHARSET_INFO *tocs)
{
  DBUG_ENTER("Item_param::set_str");
  DBUG_ASSERT(value.type_handler()->cmp_type() == STRING_RESULT);
  /*
    Assign string with no conversion: data is converted only after it's
    been written to the binary log.
  */
  uint dummy_errors;
  if (unlikely(value.m_string.copy(str, length, fromcs, tocs, &dummy_errors)))
    DBUG_RETURN(TRUE);
  /*
    Set str_value_ptr to make sure it's in sync with str_value.
    This is needed in case if we're called from Item_param::set_value(),
    from the code responsible for setting OUT parameters in
    sp_head::execute_procedure(). This makes sure that
    Protocol_binary::send_out_parameters() later gets a valid value
    from Item_param::val_str().
    Note, for IN parameters, Item_param::convert_str_value() will be called
    later, which will convert the value from the client character set to the
    connection character set, and will reset both str_value and str_value_ptr.
  */
  value.m_string_ptr.set(value.m_string.ptr(),
                         value.m_string.length(),
                         value.m_string.charset());
  state= SHORT_DATA_VALUE;
  collation.set(tocs, DERIVATION_COERCIBLE);
  max_length= length;
  base_flags&= ~item_base_t::MAYBE_NULL;
  null_value= 0;
  /* max_length and decimals are set after charset conversion */
  /* sic: str may be not null-terminated, don't add DBUG_PRINT here */
  DBUG_RETURN(FALSE);
}


bool Item_param::set_longdata(const char *str, ulong length)
{
  DBUG_ENTER("Item_param::set_longdata");
  DBUG_ASSERT(value.type_handler()->cmp_type() == STRING_RESULT);

  /*
    If client character set is multibyte, end of long data packet
    may hit at the middle of a multibyte character.  Additionally,
    if binary log is open we must write long data value to the
    binary log in character set of client. This is why we can't
    convert long data to connection character set as it comes
    (here), and first have to concatenate all pieces together,
    write query to the binary log and only then perform conversion.
  */
  if (value.m_string.length() + length > current_thd->variables.max_allowed_packet)
  {
    my_message(ER_UNKNOWN_ERROR,
               "Parameter of prepared statement which is set through "
               "mysql_send_long_data() is longer than "
               "'max_allowed_packet' bytes",
               MYF(0));
    DBUG_RETURN(true);
  }

  if (value.m_string.append(str, length, &my_charset_bin))
    DBUG_RETURN(TRUE);
  state= LONG_DATA_VALUE;
  base_flags&= ~item_base_t::MAYBE_NULL;
  null_value= 0;

  DBUG_RETURN(FALSE);
}


void Item_param::CONVERSION_INFO::set(THD *thd, CHARSET_INFO *fromcs)
{
  CHARSET_INFO *tocs= thd->variables.collation_connection;

  character_set_of_placeholder= fromcs;
  character_set_client= thd->variables.character_set_client;
  /*
    Setup source and destination character sets so that they
    are different only if conversion is necessary: this will
    make later checks easier.
  */
  uint32 dummy_offset;
  final_character_set_of_str_value=
    String::needs_conversion(0, fromcs, tocs, &dummy_offset) ?
    tocs : fromcs;
}


bool Item_param::CONVERSION_INFO::convert(THD *thd, String *str)
{
  return thd->convert_string(str,
                             character_set_of_placeholder,
                             final_character_set_of_str_value);
}


/**
  Set parameter value from Item.

  @param thd   Current thread
  @param item  Item

  @retval
    0 OK
  @retval
    1 Out of memory
*/

bool Item_param::set_from_item(THD *thd, Item *item)
{
  DBUG_ENTER("Item_param::set_from_item");
  m_is_settable_routine_parameter= item->get_settable_routine_parameter();
  if (limit_clause_param)
  {
    longlong val= item->val_int();
    if (item->null_value)
    {
      set_null();
      DBUG_RETURN(false);
    }
    else
    {
      unsigned_flag= item->unsigned_flag;
      set_handler(item->type_handler());
      DBUG_RETURN(set_limit_clause_param(val));
    }
  }
  st_value tmp;
  if (!item->save_in_value(thd, &tmp))
  {
    const Type_handler *h= item->type_handler();
    set_handler(h);
    DBUG_RETURN(set_value(thd, item, &tmp, h));
  }
  else
    set_null();

  DBUG_RETURN(0);
}

/**
  Resets parameter after execution.

  @note
    We clear null_value here instead of setting it in set_* methods,
    because we want more easily handle case for long data.
*/

void Item_param::reset()
{
  DBUG_ENTER("Item_param::reset");
  /* Shrink string buffer if it's bigger than max possible CHAR column */
  if (value.m_string.alloced_length() > MAX_CHAR_WIDTH)
    value.m_string.free();
  else
    value.m_string.length(0);
  value.m_string_ptr.length(0);
  /*
    We must prevent all charset conversions until data has been written
    to the binary log.
  */
  value.m_string.set_charset(&my_charset_bin);
  collation.set(&my_charset_bin, DERIVATION_COERCIBLE);
  state= NO_VALUE;
  set_maybe_null();
  null_value= 0;
  DBUG_VOID_RETURN;
}


int Item_param::save_in_field(Field *field, bool no_conversions)
{
  field->set_notnull();

  /*
    There's no "default" intentionally, to make compiler complain
    when adding a new XXX_VALUE value.
    Garbage (e.g. in case of a memory overrun) is handled after the switch.
  */
  switch (state) {
  case SHORT_DATA_VALUE:
  case LONG_DATA_VALUE:
    return value.type_handler()->Item_save_in_field(this, field, no_conversions);
  case NULL_VALUE:
    return set_field_to_null_with_conversions(field, no_conversions);
  case DEFAULT_VALUE:
    return field->save_in_field_default_value(field->table->pos_in_table_list->
                                              top_table() !=
                                              field->table->pos_in_table_list);
  case IGNORE_VALUE:
    return field->save_in_field_ignore_value(field->table->pos_in_table_list->
                                             top_table() !=
                                             field->table->pos_in_table_list);
  case NO_VALUE:
    DBUG_ASSERT(0); // Should not be possible
    return true;
  }
  DBUG_ASSERT(0); // Garbage
  return 1;
}


bool Item_param::is_evaluable_expression() const
{
  switch (state) {
  case SHORT_DATA_VALUE:
  case LONG_DATA_VALUE:
  case NULL_VALUE:
    return true;
  case NO_VALUE:
    return true; // Not assigned yet, so we don't know
  case IGNORE_VALUE:
  case DEFAULT_VALUE:
    break;
  }
  return false;
}


bool Item_param::can_return_value() const
{
  // There's no "default". See comments in Item_param::save_in_field().
  switch (state) {
  case SHORT_DATA_VALUE:
  case LONG_DATA_VALUE:
    return true;
  case IGNORE_VALUE:
  case DEFAULT_VALUE:
    invalid_default_param();
    // fall through
  case NULL_VALUE:
    return false;
  case NO_VALUE:
    DBUG_ASSERT(0); // Should not be possible
    return false;
  }
  DBUG_ASSERT(0); // Garbage
  return false;
}


void Item_param::invalid_default_param() const
{
  my_message(ER_INVALID_DEFAULT_PARAM,
             ER_THD(current_thd, ER_INVALID_DEFAULT_PARAM), MYF(0));
}


bool Item_param::get_date(THD *thd, MYSQL_TIME *res, date_mode_t fuzzydate)
{
  /*
    LIMIT clause parameter should not call get_date()
    For non-LIMIT parameters, handlers must be the same.
  */
  DBUG_ASSERT(type_handler()->result_type() ==
              value.type_handler()->result_type());
  if (state == SHORT_DATA_VALUE &&
      value.type_handler()->cmp_type() == TIME_RESULT)
  {
    *res= value.time;
    return 0;
  }
  return type_handler()->Item_get_date_with_warn(thd, this, res, fuzzydate);
}


double Item_param::PValue::val_real(const Type_std_attributes *attr) const
{
  switch (type_handler()->cmp_type()) {
  case REAL_RESULT:
    return real;
  case INT_RESULT:
    return attr->unsigned_flag
      ? (double) (ulonglong) integer
      : (double) integer;
  case DECIMAL_RESULT:
    return m_decimal.to_double();
  case STRING_RESULT:
    return double_from_string_with_check(&m_string);
  case TIME_RESULT:
    /*
      This works for example when user says SELECT ?+0.0 and supplies
      time value for the placeholder.
    */
    return TIME_to_double(&time);
  case ROW_RESULT:
    DBUG_ASSERT(0);
    break;
  }
  return 0.0;
}


longlong Item_param::PValue::val_int(const Type_std_attributes *attr) const
{
  switch (type_handler()->cmp_type()) {
  case REAL_RESULT:
    return Converter_double_to_longlong(real, attr->unsigned_flag).result();
  case INT_RESULT:
    return integer;
  case DECIMAL_RESULT:
    return m_decimal.to_longlong(attr->unsigned_flag);
  case STRING_RESULT:
    return longlong_from_string_with_check(&m_string);
  case TIME_RESULT:
    return (longlong) TIME_to_ulonglong(&time);
  case ROW_RESULT:
    DBUG_ASSERT(0);
    break;
  }
  return 0;
}


my_decimal *Item_param::PValue::val_decimal(my_decimal *dec,
                                            const Type_std_attributes *attr)
{
  switch (type_handler()->cmp_type()) {
  case DECIMAL_RESULT:
    return &m_decimal;
  case REAL_RESULT:
    double2my_decimal(E_DEC_FATAL_ERROR, real, dec);
    return dec;
  case INT_RESULT:
    int2my_decimal(E_DEC_FATAL_ERROR, integer, attr->unsigned_flag, dec);
    return dec;
  case STRING_RESULT:
    return decimal_from_string_with_check(dec, &m_string);
  case TIME_RESULT:
    return TIME_to_my_decimal(&time, dec);
  case ROW_RESULT:
    DBUG_ASSERT(0);
    break;
  }
  return 0;
}


String *Item_param::PValue::val_str(String *str,
                                    const Type_std_attributes *attr)
{
  switch (type_handler()->cmp_type()) {
  case STRING_RESULT:
    return &m_string_ptr;
  case REAL_RESULT:
    str->set_real(real, NOT_FIXED_DEC, &my_charset_bin);
    return str;
  case INT_RESULT:
    str->set_int(integer, attr->unsigned_flag, &my_charset_bin);
    return str;
  case DECIMAL_RESULT:
    if (m_decimal.to_string_native(str, 0, 0, 0) <= 1)
      return str;
    return NULL;
  case TIME_RESULT:
  {
    if (str->reserve(MAX_DATE_STRING_REP_LENGTH))
      return NULL;
    str->length((uint) my_TIME_to_str(&time, (char*) str->ptr(),
                attr->decimals));
    str->set_charset(&my_charset_bin);
    return str;
  }
  case ROW_RESULT:
    DBUG_ASSERT(0);
    break;
  }
  return NULL;
}


/**
  Return Param item values in string format, for generating the dynamic 
  query used in update/binary logs.

  @todo
    - Change interface and implementation to fill log data in place
    and avoid one more memcpy/alloc between str and log string.
    - In case of error we need to notify replication
    that binary log contains wrong statement 
*/

const String *Item_param::value_query_val_str(THD *thd, String *str) const
{
  switch (value.type_handler()->cmp_type()) {
  case INT_RESULT:
    str->set_int(value.integer, unsigned_flag, &my_charset_bin);
    return str;
  case REAL_RESULT:
    str->set_real(value.real, NOT_FIXED_DEC, &my_charset_bin);
    return str;
  case DECIMAL_RESULT:
    if (value.m_decimal.to_string_native(str, 0, 0, 0) > 1)
      return &my_null_string;
    return str;
  case TIME_RESULT:
    {
      static const uint32 typelen= 9; // "TIMESTAMP" is the longest type name
      char *buf, *ptr;
      str->length(0);
      /*
        TODO: in case of error we need to notify replication
        that binary log contains wrong statement
      */
      if (str->reserve(MAX_DATE_STRING_REP_LENGTH + 3 + typelen))
        return NULL;

      /* Create date string inplace */
      switch (value.time.time_type) {
      case MYSQL_TIMESTAMP_DATE:
        str->append(STRING_WITH_LEN("DATE"));
        break;
      case MYSQL_TIMESTAMP_TIME:
        str->append(STRING_WITH_LEN("TIME"));
        break;
      case MYSQL_TIMESTAMP_DATETIME:
        str->append(STRING_WITH_LEN("TIMESTAMP"));
        break;
      case MYSQL_TIMESTAMP_ERROR:
      case MYSQL_TIMESTAMP_NONE:
        break;
      }
      DBUG_ASSERT(str->length() <= typelen);
      buf= (char*) str->ptr();
      ptr= buf + str->length();
      *ptr++= '\'';
      ptr+= (uint) my_TIME_to_str(&value.time, ptr, decimals);
      *ptr++= '\'';
      str->length((uint32) (ptr - buf));
      return str;
    }
  case STRING_RESULT:
    {
      str->length(0);
      append_query_string(value.cs_info.character_set_client, str,
                          value.m_string.ptr(), value.m_string.length(),
                          thd->variables.sql_mode & MODE_NO_BACKSLASH_ESCAPES);
      return str;
    }
  case ROW_RESULT:
    DBUG_ASSERT(0);
    break;
  }
  return NULL;
}


const String *Item_param::query_val_str(THD *thd, String* str) const
{
  // There's no "default". See comments in Item_param::save_in_field().
  switch (state) {
  case SHORT_DATA_VALUE:
  case LONG_DATA_VALUE:
    return value_query_val_str(thd, str);
  case IGNORE_VALUE:
  case DEFAULT_VALUE:
    return &my_default_string;
  case NULL_VALUE:
    return &my_null_string;
  case NO_VALUE:
    DBUG_ASSERT(0); // Should not be possible
    return NULL;
  }
  DBUG_ASSERT(0); // Garbage
  return NULL;
}


/**
  Convert string from client character set to the character set of
  connection.
*/

bool Item_param::convert_str_value(THD *thd)
{
  bool rc= FALSE;
  if ((state == SHORT_DATA_VALUE || state == LONG_DATA_VALUE) &&
      value.type_handler()->cmp_type() == STRING_RESULT)
  {
    rc= value.cs_info.convert_if_needed(thd, &value.m_string);
    /* Here str_value is guaranteed to be in final_character_set_of_str_value */

    /*
      str_value_ptr is returned from val_str(). It must be not alloced
      to prevent it's modification by val_str() invoker.
    */
    value.m_string_ptr.set(value.m_string.ptr(), value.m_string.length(),
                           value.m_string.charset());
    /* Synchronize item charset and length with value charset */
    fix_charset_and_length_from_str_value(value.m_string, DERIVATION_COERCIBLE);
  }
  return rc;
}


bool Item_param::basic_const_item() const
{
  switch (state) {
  case LONG_DATA_VALUE:
  case NULL_VALUE:
    return true;
  case SHORT_DATA_VALUE:
    return type_handler()->cmp_type() != TIME_RESULT;
  case DEFAULT_VALUE:
  case IGNORE_VALUE:
    invalid_default_param();
    return false;
  case NO_VALUE:
    break;
  }
  return false;
}


Item *Item_param::value_clone_item(THD *thd)
{
  MEM_ROOT *mem_root= thd->mem_root;
  switch (value.type_handler()->cmp_type()) {
  case INT_RESULT:
    return (unsigned_flag ?
            new (mem_root) Item_uint(thd, name.str, value.integer, max_length) :
            new (mem_root) Item_int(thd, name.str, value.integer, max_length));
  case REAL_RESULT:
    return new (mem_root) Item_float(thd, name.str, value.real, decimals,
                                     max_length);
  case DECIMAL_RESULT:
    return 0; // Should create Item_decimal. See MDEV-11361.
  case STRING_RESULT:
    return new (mem_root) Item_string(thd, name,
                                      Lex_cstring(value.m_string.ptr(),
                                                  value.m_string.length()),
                                      value.m_string.charset(),
                                      collation.derivation,
                                      collation.repertoire);
  case TIME_RESULT:
    break;
  case ROW_RESULT:
    DBUG_ASSERT(0);
    break;
  }
  return 0;
}


/* see comments in the header file */

Item *
Item_param::clone_item(THD *thd)
{
  // There's no "default". See comments in Item_param::save_in_field().
  switch (state) {
  case IGNORE_VALUE:
  case DEFAULT_VALUE:
    invalid_default_param();
    // fall through
  case NULL_VALUE:
    return new (thd->mem_root) Item_null(thd, name.str);
  case SHORT_DATA_VALUE:
  case LONG_DATA_VALUE:
  {
    DBUG_ASSERT(type_handler()->cmp_type() == value.type_handler()->cmp_type());
    return value_clone_item(thd);
  }
  case NO_VALUE:
    return 0;
  }
  DBUG_ASSERT(0);  // Garbage
  return 0;
}


/* End of Item_param related */

void Item_param::print(String *str, enum_query_type query_type)
{
  if (state == NO_VALUE)
  {
    str->append('?');
  }
  else if (state == DEFAULT_VALUE)
  {
    str->append(STRING_WITH_LEN("default"));
  }
  else if (state == IGNORE_VALUE)
  {
    str->append(STRING_WITH_LEN("ignore"));
  }
  else
  {
    char buffer[STRING_BUFFER_USUAL_SIZE];
    String tmp(buffer, sizeof(buffer), &my_charset_bin);
    const String *res;
    res= query_val_str(current_thd, &tmp);
    str->append(*res);
  }
}


/**
  Preserve the original parameter types and values
  when re-preparing a prepared statement.

  @details Copy parameter type information and conversion
  function pointers from a parameter of the old statement
  to the corresponding parameter of the new one.

  Move parameter values from the old parameters to the new
  one. We simply "exchange" the values, which allows
  to save on allocation and character set conversion in
  case a parameter is a string or a blob/clob.

  The old parameter gets the value of this one, which
  ensures that all memory of this parameter is freed
  correctly.

  @param[in]  src   parameter item of the original
                    prepared statement
*/

void
Item_param::set_param_type_and_swap_value(Item_param *src)
{
  Type_std_attributes::set(src);
  set_handler(src->type_handler());

  copy_flags(src, item_base_t::MAYBE_NULL);
  null_value= src->null_value;
  state= src->state;

  value.swap(src->value);
}


void Item_param::set_default()
{
  m_is_settable_routine_parameter= false;
  state= DEFAULT_VALUE;
  /*
    When Item_param is set to DEFAULT_VALUE:
    - its val_str() and val_decimal() return NULL
    - get_date() returns true
    It's important also to have null_value==true for DEFAULT_VALUE.
    Otherwise the callers of val_xxx() and get_date(), e.g. Item::send(),
    can misbehave (e.g. crash on asserts).
  */
  null_value= true;
}

void Item_param::set_ignore()
{
  m_is_settable_routine_parameter= false;
  state= IGNORE_VALUE;
  null_value= true;
}

/**
  This operation is intended to store some item value in Item_param to be
  used later.

  @param thd    thread context
  @param ctx    stored procedure runtime context
  @param it     a pointer to an item in the tree

  @return Error status
    @retval TRUE on error
    @retval FALSE on success
*/

bool
Item_param::set_value(THD *thd, sp_rcontext *ctx, Item **it)
{
  Item *arg= *it;
  st_value tmp;
  /*
    The OUT parameter is bound to some data type.
    It's important not to touch m_type_handler,
    to make sure the next mysql_stmt_execute()
    correctly fetches the value from the client-server protocol,
    using set_param_func().
  */
  if (arg->save_in_value(thd, &tmp) ||
      set_value(thd, arg, &tmp, arg->type_handler()))
  {
    set_null();
    return false;
  }
  /* It is wrapper => other set_* shoud set null_value */
  DBUG_ASSERT(null_value == false);
  return false;
}


/**
  Setter of Item_param::m_out_param_info.

  m_out_param_info is used to store information about store routine
  OUT-parameters, such as stored routine name, database, stored routine
  variable name. It is supposed to be set in sp_head::execute() after
  Item_param::set_value() is called.
*/

void
Item_param::set_out_param_info(Send_field *info)
{
  m_out_param_info= info;
  set_handler(m_out_param_info->type_handler());
}


/**
  Getter of Item_param::m_out_param_info.

  m_out_param_info is used to store information about store routine
  OUT-parameters, such as stored routine name, database, stored routine
  variable name. It is supposed to be retrieved in
  Protocol_binary::send_out_parameters() during creation of OUT-parameter
  result set.
*/

const Send_field *
Item_param::get_out_param_info() const
{
  return m_out_param_info;
}


/**
  Fill meta-data information for the corresponding column in a result set.
  If this is an OUT-parameter of a stored procedure, preserve meta-data of
  stored-routine variable.

  @param field container for meta-data to be filled
*/

void Item_param::make_send_field(THD *thd, Send_field *field)
{
  Item::make_send_field(thd, field);

  if (!m_out_param_info)
    return;

  /*
    This is an OUT-parameter of stored procedure. We should use
    OUT-parameter info to fill out the names.
  */

  *field= *m_out_param_info;
}

bool Item_param::append_for_log(THD *thd, String *str)
{
  StringBuffer<STRING_BUFFER_USUAL_SIZE> buf;
  const String *val= query_val_str(thd, &buf);
  return str->append(*val);
}


/****************************************************************************
  Item_copy_string
****************************************************************************/

double Item_copy_string::val_real()
{
  int err_not_used;
  char *end_not_used;
  return (null_value ? 0.0 :
          str_value.charset()->strntod((char*) str_value.ptr(),
                                       str_value.length(),
                                       &end_not_used, &err_not_used));
}

longlong Item_copy_string::val_int()
{
  int err;
  return null_value ? 0 : str_value.charset()->strntoll(str_value.ptr(),
                                                        str_value.length(), 10,
                                                        (char**) 0, &err);
}


int Item_copy_string::save_in_field(Field *field, bool no_conversions)
{
  return save_str_value_in_field(field, &str_value);
}


void Item_copy_string::copy()
{
  String *res=item->val_str(&str_value);
  if (res && res != &str_value)
    str_value.copy(*res);
  null_value=item->null_value;
}

/* ARGSUSED */
String *Item_copy_string::val_str(String *str)
{
  // Item_copy_string is used without fix_fields call
  if (null_value)
    return (String*) 0;
  return &str_value;
}


my_decimal *Item_copy_string::val_decimal(my_decimal *decimal_value)
{
  // Item_copy_string is used without fix_fields call
  if (null_value)
    return (my_decimal *) 0;
  string2my_decimal(E_DEC_FATAL_ERROR, &str_value, decimal_value);
  return (decimal_value);
}


/*
  Functions to convert item to field (for send_result_set_metadata)
*/

void Item_ref_null_helper::save_val(Field *to)
{
  DBUG_ASSERT(fixed());
  (*ref)->save_val(to);
  owner->was_null|= null_value= (*ref)->null_value;
}


double Item_ref_null_helper::val_real()
{
  DBUG_ASSERT(fixed());
  double tmp= (*ref)->val_result();
  owner->was_null|= null_value= (*ref)->null_value;
  return tmp;
}


longlong Item_ref_null_helper::val_int()
{
  DBUG_ASSERT(fixed());
  longlong tmp= (*ref)->val_int_result();
  owner->was_null|= null_value= (*ref)->null_value;
  return tmp;
}


my_decimal *Item_ref_null_helper::val_decimal(my_decimal *decimal_value)
{
  DBUG_ASSERT(fixed());
  my_decimal *val= (*ref)->val_decimal_result(decimal_value);
  owner->was_null|= null_value= (*ref)->null_value;
  return val;
}


bool Item_ref_null_helper::val_bool()
{
  DBUG_ASSERT(fixed());
  bool val= (*ref)->val_bool_result();
  owner->was_null|= null_value= (*ref)->null_value;
  return val;
}


String* Item_ref_null_helper::val_str(String* s)
{
  DBUG_ASSERT(fixed());
  String* tmp= (*ref)->str_result(s);
  owner->was_null|= null_value= (*ref)->null_value;
  return tmp;
}


bool Item_ref_null_helper::val_native(THD *thd, Native *to)
{
  return (owner->was_null|= val_native_from_item(thd, *ref, to));
}


bool Item_ref_null_helper::get_date(THD *thd, MYSQL_TIME *ltime,
                                    date_mode_t fuzzydate)
{  
  return (owner->was_null|= null_value= (*ref)->get_date_result(thd, ltime,
                                                                fuzzydate));
}


/**
  Mark item and SELECT_LEXs as dependent if item was resolved in
  outer SELECT.

  @param thd             thread handler
  @param last            select from which current item depend
  @param current         current select
  @param resolved_item   item which was resolved in outer SELECT(for warning)
  @param mark_item       item which should be marked (can be differ in case of
                         substitution)
  @param suppress_warning_output  flag specifying whether to suppress output of
                                  a warning message
*/

static bool mark_as_dependent(THD *thd, SELECT_LEX *last, SELECT_LEX *current,
                              Item_ident *resolved_item,
                              Item_ident *mark_item,
                              bool suppress_warning_output)
{
  DBUG_ENTER("mark_as_dependent");
  DBUG_PRINT("info", ("current select: %d (%p)  last: %d (%p)",
                      current->select_number, current,
                      (last ? last->select_number : 0), last));

  /* store pointer on SELECT_LEX from which item is dependent */
  if (mark_item && mark_item->can_be_depended)
  {
    DBUG_PRINT("info", ("mark_item: %p  lex: %p", mark_item, last));
    mark_item->depended_from= last;
  }
  if (current->mark_as_dependent(thd, last,
                                 /** resolved_item psergey-thu **/ mark_item))
    DBUG_RETURN(TRUE);
  if ((thd->lex->describe & DESCRIBE_EXTENDED) && !suppress_warning_output)
  {
    const char *db_name= (resolved_item->db_name.str ?
                          resolved_item->db_name.str : "");
    const char *table_name= (resolved_item->table_name.str ?
                             resolved_item->table_name.str : "");
    push_warning_printf(thd, Sql_condition::WARN_LEVEL_NOTE,
                        ER_WARN_FIELD_RESOLVED,
                        ER_THD(thd,ER_WARN_FIELD_RESOLVED),
                        db_name, (db_name[0] ? "." : ""),
                        table_name, (table_name [0] ? "." : ""),
                        resolved_item->field_name.str,
                        current->select_number, last->select_number);
  }
  DBUG_RETURN(FALSE);
}


/**
  Mark range of selects and resolved identifier (field/reference)
  item as dependent.

  @param thd             thread handler
  @param last_select     select where resolved_item was resolved
  @param current_sel     current select (select where resolved_item was placed)
  @param found_field     field which was found during resolving
  @param found_item      Item which was found during resolving (if resolved
                         identifier belongs to VIEW)
  @param resolved_item   Identifier which was resolved
  @param suppress_warning_output  flag specifying whether to suppress output of
                                  a warning message

  @note
    We have to mark all items between current_sel (including) and
    last_select (excluding) as dependent (select before last_select should
    be marked with actual table mask used by resolved item, all other with
    OUTER_REF_TABLE_BIT) and also write dependence information to Item of
    resolved identifier.
*/

void mark_select_range_as_dependent(THD *thd, SELECT_LEX *last_select,
                                    SELECT_LEX *current_sel,
                                    Field *found_field, Item *found_item,
                                    Item_ident *resolved_item,
                                    bool suppress_warning_output)
{
  /*
    Go from current SELECT to SELECT where field was resolved (it
    have to be reachable from current SELECT, because it was already
    done once when we resolved this field and cached result of
    resolving)
  */
  SELECT_LEX *previous_select= current_sel;
  for (; previous_select->context.outer_select() != last_select;
       previous_select= previous_select->context.outer_select())
  {
    Item_subselect *prev_subselect_item=
      previous_select->master_unit()->item;
    prev_subselect_item->used_tables_cache|= OUTER_REF_TABLE_BIT;
    prev_subselect_item->const_item_cache= 0;
  }

  Item_subselect *prev_subselect_item=
    previous_select->master_unit()->item;
  Item_ident *dependent= resolved_item;
  if (found_field == view_ref_found)
  {
    Item::Type type= found_item->type();
    prev_subselect_item->used_tables_cache|=
      found_item->used_tables();
    dependent= ((type == Item::REF_ITEM || type == Item::FIELD_ITEM) ?
                (Item_ident*) found_item :
                0);
  }
  else
    prev_subselect_item->used_tables_cache|=
      found_field->table->map;
  prev_subselect_item->const_item_cache= 0;
  mark_as_dependent(thd, last_select, current_sel, resolved_item,
                    dependent, suppress_warning_output);
}


/**
  Search a GROUP BY clause for a field with a certain name.

  Search the GROUP BY list for a column named as find_item. When searching
  preference is given to columns that are qualified with the same table (and
  database) name as the one being searched for.

  @param find_item     the item being searched for
  @param group_list    GROUP BY clause

  @return
    - the found item on success
    - NULL if find_item is not in group_list
*/

static Item** find_field_in_group_list(Item *find_item, ORDER *group_list)
{
  LEX_CSTRING db_name;
  LEX_CSTRING table_name;
  LEX_CSTRING field_name;
  ORDER      *found_group= NULL;
  int         found_match_degree= 0;
  char        name_buff[SAFE_NAME_LEN+1];

  if (find_item->type() == Item::FIELD_ITEM ||
      find_item->type() == Item::REF_ITEM)
  {
    db_name=    ((Item_ident*) find_item)->db_name;
    table_name= ((Item_ident*) find_item)->table_name;
    field_name= ((Item_ident*) find_item)->field_name;
  }
  else
    return NULL;

  if (db_name.str && lower_case_table_names)
  {
    /* Convert database to lower case for comparison */
    strmake_buf(name_buff, db_name.str);
    my_casedn_str(files_charset_info, name_buff);
    db_name= Lex_cstring_strlen(name_buff);
  }

  DBUG_ASSERT(field_name.str != 0);

  for (ORDER *cur_group= group_list ; cur_group ; cur_group= cur_group->next)
  {
    int cur_match_degree= 0;

    /* SELECT list element with explicit alias */
    if ((*(cur_group->item))->name.str && !table_name.str &&
        (*(cur_group->item))->is_explicit_name() &&
        !lex_string_cmp(system_charset_info,
                        &(*(cur_group->item))->name, &field_name))
    {
      ++cur_match_degree;
    }
    /* Reference on the field or view/derived field. */
    else if ((*(cur_group->item))->type() == Item::FIELD_ITEM ||
             (*(cur_group->item))->type() == Item::REF_ITEM )
    {
      Item_ident *cur_field= (Item_ident*) *cur_group->item;
      const char *l_db_name= cur_field->db_name.str;
      const char *l_table_name= cur_field->table_name.str;
      LEX_CSTRING *l_field_name= &cur_field->field_name;

      DBUG_ASSERT(l_field_name->str != 0);

      if (!lex_string_cmp(system_charset_info,
                          l_field_name, &field_name))
        ++cur_match_degree;
      else
        continue;

      if (l_table_name && table_name.str)
      {
        /* If field_name is qualified by a table name. */
        if (my_strcasecmp(table_alias_charset, l_table_name, table_name.str))
          /* Same field names, different tables. */
          return NULL;

        ++cur_match_degree;
        if (l_db_name && db_name.str)
        {
          /* If field_name is also qualified by a database name. */
          if (strcmp(l_db_name, db_name.str))
            /* Same field names, different databases. */
            return NULL;
          ++cur_match_degree;
        }
      }
    }
    else
      continue;

    if (cur_match_degree > found_match_degree)
    {
      found_match_degree= cur_match_degree;
      found_group= cur_group;
    }
    else if (found_group && (cur_match_degree == found_match_degree) &&
             !(*(found_group->item))->eq((*(cur_group->item)), 0))
    {
      /*
        If the current resolve candidate matches equally well as the current
        best match, they must reference the same column, otherwise the field
        is ambiguous.
      */
      my_error(ER_NON_UNIQ_ERROR, MYF(0),
               find_item->full_name(), current_thd->where);
      return NULL;
    }
  }

  if (found_group)
    return found_group->item;
  else
    return NULL;
}


/**
  Resolve a column reference in a sub-select.

  Resolve a column reference (usually inside a HAVING clause) against the
  SELECT and GROUP BY clauses of the query described by 'select'. The name
  resolution algorithm searches both the SELECT and GROUP BY clauses, and in
  case of a name conflict prefers GROUP BY column names over SELECT names. If
  both clauses contain different fields with the same names, a warning is
  issued that name of 'ref' is ambiguous. We extend ANSI SQL in that when no
  GROUP BY column is found, then a HAVING name is resolved as a possibly
  derived SELECT column. This extension is allowed only if the
  MODE_ONLY_FULL_GROUP_BY sql mode isn't enabled.

  @param thd     current thread
  @param ref     column reference being resolved
  @param select  the select that ref is resolved against

  @note
    The resolution procedure is:
    - Search for a column or derived column named col_ref_i [in table T_j]
    in the SELECT clause of Q.
    - Search for a column named col_ref_i [in table T_j]
    in the GROUP BY clause of Q.
    - If found different columns with the same name in GROUP BY and SELECT:
    - if the condition that uses this column name is pushed down into
    the HAVING clause return the SELECT column
    - else issue a warning and return the GROUP BY column.
    - Otherwise
    - if the MODE_ONLY_FULL_GROUP_BY mode is enabled return error
    - else return the found SELECT column.


  @return
    - NULL - there was an error, and the error was already reported
    - not_found_item - the item was not resolved, no error was reported
    - resolved item - if the item was resolved
*/

static Item**
resolve_ref_in_select_and_group(THD *thd, Item_ident *ref, SELECT_LEX *select)
{
  Item **group_by_ref= NULL;
  Item **select_ref= NULL;
  ORDER *group_list= select->group_list.first;
  bool ambiguous_fields= FALSE;
  uint counter;
  enum_resolution_type resolution;

  /*
    Search for a column or derived column named as 'ref' in the SELECT
    clause of the current select.
  */
  if (!(select_ref= find_item_in_list(ref, *(select->get_item_list()),
                                      &counter, REPORT_EXCEPT_NOT_FOUND,
                                      &resolution)))
    return NULL; /* Some error occurred. */
  if (resolution == RESOLVED_AGAINST_ALIAS)
    ref->alias_name_used= TRUE;

  /* If this is a non-aggregated field inside HAVING, search in GROUP BY. */
  if (select->having_fix_field && !ref->with_sum_func() && group_list)
  {
    group_by_ref= find_field_in_group_list(ref, group_list);
    
    /* Check if the fields found in SELECT and GROUP BY are the same field. */
    if (group_by_ref && (select_ref != not_found_item) &&
        !((*group_by_ref)->eq(*select_ref, 0)) &&
        (!select->having_fix_field_for_pushed_cond))
    {
      ambiguous_fields= TRUE;
      push_warning_printf(thd, Sql_condition::WARN_LEVEL_WARN,
                          ER_NON_UNIQ_ERROR,
                          ER_THD(thd,ER_NON_UNIQ_ERROR), ref->full_name(),
                          thd->where);

    }
  }

  if (thd->variables.sql_mode & MODE_ONLY_FULL_GROUP_BY &&
      select->having_fix_field  &&
      select_ref != not_found_item && !group_by_ref &&
      !ref->alias_name_used)
  {
    /*
      Report the error if fields was found only in the SELECT item list and
      the strict mode is enabled.
    */
    my_error(ER_NON_GROUPING_FIELD_USED, MYF(0),
             ref->name.str, "HAVING");
    return NULL;
  }
  if (select_ref != not_found_item || group_by_ref)
  {
    if (select_ref != not_found_item && !ambiguous_fields)
    {
      DBUG_ASSERT(*select_ref != 0);
      if (!select->ref_pointer_array[counter])
      {
        my_error(ER_ILLEGAL_REFERENCE, MYF(0),
                 ref->name.str, "forward reference in item list");
        return NULL;
      }
      DBUG_ASSERT((*select_ref)->fixed());
      return &select->ref_pointer_array[counter];
    }
    if (group_by_ref)
      return group_by_ref;
    DBUG_ASSERT(FALSE);
    return NULL; /* So there is no compiler warning. */
  }

  return (Item**) not_found_item;
}


/*
  @brief
  Whether a table belongs to an outer select.

  @param table table to check
  @param select current select

  @details
  Try to find select the table belongs to by ascending the derived tables chain.
*/

static
bool is_outer_table(TABLE_LIST *table, SELECT_LEX *select)
{
  DBUG_ASSERT(table->select_lex != select);
  TABLE_LIST *tl;

  if (table->belong_to_view &&
      table->belong_to_view->select_lex == select)
    return FALSE;

  for (tl= select->master_unit()->derived;
       tl && tl->is_merged_derived();
       select= tl->select_lex, tl= select->master_unit()->derived)
  {
    if (tl->select_lex == table->select_lex)
      return FALSE;
  }
  return TRUE;
}


/**
  Resolve the name of an outer select column reference.

  @param[in] thd             current thread
  @param[in,out] from_field  found field reference or (Field*)not_found_field
  @param[in,out] reference   view column if this item was resolved to a
    view column

  @description
  The method resolves the column reference represented by 'this' as a column
  present in outer selects that contain current select.

  In prepared statements, because of cache, find_field_in_tables()
  can resolve fields even if they don't belong to current context.
  In this case this method only finds appropriate context and marks
  current select as dependent. The found reference of field should be
  provided in 'from_field'.

  The cache is critical for prepared statements of type:

  SELECT a FROM (SELECT a FROM test.t1) AS s1 NATURAL JOIN t2 AS s2;

  This is internally converted to a join similar to

  SELECT a FROM t1 AS s1,t2 AS s2 WHERE t2.a=t1.a;

  Without the cache, we would on re-prepare not know if 'a' did match
  s1.a or s2.a.

  @note
    This is the inner loop of Item_field::fix_fields:
  @code
        for each outer query Q_k beginning from the inner-most one
        {
          search for a column or derived column named col_ref_i
          [in table T_j] in the FROM clause of Q_k;

          if such a column is not found
            Search for a column or derived column named col_ref_i
            [in table T_j] in the SELECT and GROUP clauses of Q_k.
        }
  @endcode

  @retval
    1   column succefully resolved and fix_fields() should continue.
  @retval
    0   column fully fixed and fix_fields() should return FALSE
  @retval
    -1  error occurred
*/

int
Item_field::fix_outer_field(THD *thd, Field **from_field, Item **reference)
{
  enum_parsing_place place= NO_MATTER;
  bool field_found= (*from_field != not_found_field);
  bool upward_lookup= FALSE;
  TABLE_LIST *table_list;

  /* Calculate the TABLE_LIST for the table */
  table_list= (cached_table ? cached_table :
               field_found && (*from_field) != view_ref_found ?
               (*from_field)->table->pos_in_table_list : 0);
  /*
    If there are outer contexts (outer selects, but current select is
    not derived table or view) try to resolve this reference in the
    outer contexts.

    We treat each subselect as a separate namespace, so that different
    subselects may contain columns with the same names. The subselects
    are searched starting from the innermost.
  */
  Name_resolution_context *last_checked_context= context;
  Item **ref= (Item **) not_found_item;
  SELECT_LEX *current_sel= context->select_lex;
  Name_resolution_context *outer_context= 0;
  SELECT_LEX *select= 0;

  if (current_sel->master_unit()->outer_select())
    outer_context= context->outer_context;

  /*
    This assert is to ensure we have an outer contex when *from_field
    is set.
    If this would not be the case, we would assert in mark_as_dependent
    as last_checked_countex == context
  */
  DBUG_ASSERT(outer_context || !*from_field ||
              *from_field == not_found_field);
  for (;
       outer_context;
       outer_context= outer_context->outer_context)
  {
    select= outer_context->select_lex;
    Item_subselect *prev_subselect_item=
      last_checked_context->select_lex->master_unit()->item;
    last_checked_context= outer_context;
    upward_lookup= TRUE;

    place= prev_subselect_item->parsing_place;
    /*
      If outer_field is set, field was already found by first call
      to find_field_in_tables(). Only need to find appropriate context.
    */
    if (field_found && outer_context->select_lex !=
        table_list->select_lex)
      continue;
    /*
      In case of a view, find_field_in_tables() writes the pointer to
      the found view field into '*reference', in other words, it
      substitutes this Item_field with the found expression.
    */
    if (field_found || (*from_field= find_field_in_tables(thd, this,
                                          outer_context->
                                            first_name_resolution_table,
                                          outer_context->
                                            last_name_resolution_table,
                                          outer_context->
                                            ignored_tables,
                                          reference,
                                          IGNORE_EXCEPT_NON_UNIQUE,
                                          TRUE, TRUE)) !=
        not_found_field)
    {
      if (*from_field)
      {
        if (thd->variables.sql_mode & MODE_ONLY_FULL_GROUP_BY &&
            select->cur_pos_in_select_list != UNDEF_POS)
        {
          /*
            As this is an outer field it should be added to the list of
            non aggregated fields of the outer select.
          */
          if (select->join)
          {
            marker= select->cur_pos_in_select_list;
            select->join->non_agg_fields.push_back(this, thd->mem_root);
          }
          else
          {
            /*
              join is absent if it is upper SELECT_LEX of non-select
              command
            */
            DBUG_ASSERT(select->master_unit()->outer_select() == NULL &&
                        (thd->lex->sql_command != SQLCOM_SELECT &&
                         thd->lex->sql_command != SQLCOM_UPDATE_MULTI &&
                         thd->lex->sql_command != SQLCOM_DELETE_MULTI &&
                         thd->lex->sql_command != SQLCOM_INSERT_SELECT &&
                         thd->lex->sql_command != SQLCOM_REPLACE_SELECT));
          }
        }
        if (*from_field != view_ref_found)
        {
          prev_subselect_item->used_tables_cache|= (*from_field)->table->map;
          prev_subselect_item->const_item_cache= 0;
          set_field(*from_field);
          if (!last_checked_context->select_lex->having_fix_field &&
              select->group_list.elements &&
              (place == SELECT_LIST || place == IN_HAVING))
          {
            Item_outer_ref *rf;
            /*
              If an outer field is resolved in a grouping select then it
              is replaced for an Item_outer_ref object. Otherwise an
              Item_field object is used.
              The new Item_outer_ref object is saved in the inner_refs_list of
              the outer select. Here it is only created. It can be fixed only
              after the original field has been fixed and this is done in the
              fix_inner_refs() function.
            */
            ;
            if (!(rf= new (thd->mem_root) Item_outer_ref(thd, context, this)))
              return -1;
            thd->change_item_tree(reference, rf);
            select->inner_refs_list.push_back(rf, thd->mem_root);
            rf->in_sum_func= thd->lex->in_sum_func;
          }
          /*
            A reference is resolved to a nest level that's outer or the same as
            the nest level of the enclosing set function : adjust the value of
            max_arg_level for the function if it's needed.
          */
          if (thd->lex->in_sum_func &&
              thd->lex == context->select_lex->parent_lex &&
              thd->lex->in_sum_func->nest_level >= select->nest_level)
          {
            Item::Type ref_type= (*reference)->type();
            set_if_bigger(thd->lex->in_sum_func->max_arg_level,
                          select->nest_level);
            set_field(*from_field);
            base_flags|= item_base_t::FIXED;
            mark_as_dependent(thd, last_checked_context->select_lex,
                              context->select_lex, this,
                              ((ref_type == REF_ITEM ||
                                ref_type == FIELD_ITEM) ?
                               (Item_ident*) (*reference) : 0), false);
            return 0;
          }
        }
        else
        {
          Item::Type ref_type= (*reference)->type();
          prev_subselect_item->used_tables_and_const_cache_join(*reference);
          mark_as_dependent(thd, last_checked_context->select_lex,
                            context->select_lex, this,
                            ((ref_type == REF_ITEM || ref_type == FIELD_ITEM) ?
                             (Item_ident*) (*reference) :
                             0), false);
          if (thd->lex->in_sum_func &&
              thd->lex == context->select_lex->parent_lex &&
              thd->lex->in_sum_func->nest_level >= select->nest_level)
          {
            set_if_bigger(thd->lex->in_sum_func->max_arg_level,
                          select->nest_level);
          }
          /*
            A reference to a view field had been found and we
            substituted it instead of this Item (find_field_in_tables
            does it by assigning the new value to *reference), so now
            we can return from this function.
          */
          return 0;
        }
      }
      break;
    }

    /* Search in SELECT and GROUP lists of the outer select. */
    if (place != IN_WHERE && place != IN_ON)
    {
      if (!(ref= resolve_ref_in_select_and_group(thd, this, select)))
        return -1; /* Some error occurred (e.g. ambiguous names). */
      if (ref != not_found_item)
      {
        DBUG_ASSERT(*ref && (*ref)->fixed());
        prev_subselect_item->used_tables_and_const_cache_join(*ref);
        break;
      }
    }

    /*
      Reference is not found in this select => this subquery depend on
      outer select (or we just trying to find wrong identifier, in this
      case it does not matter which used tables bits we set)
    */
    prev_subselect_item->used_tables_cache|= OUTER_REF_TABLE_BIT;
    prev_subselect_item->const_item_cache= 0;
  }

  DBUG_ASSERT(ref != 0);
  if (!*from_field)
    return -1;
  if (ref == not_found_item && *from_field == not_found_field)
  {
    if (upward_lookup)
    {
      // We can't say exactly what absent table or field
      my_error(ER_BAD_FIELD_ERROR, MYF(0), full_name(), thd->where);
    }
    else
    {
      /* Call find_field_in_tables only to report the error */
      find_field_in_tables(thd, this,
                           context->first_name_resolution_table,
                           context->last_name_resolution_table,
                           context->ignored_tables,
                           reference, REPORT_ALL_ERRORS,
                           !any_privileges, TRUE);
    }
    return -1;
  }
  else if (ref != not_found_item)
  {
    Item *save;
    Item_ref *rf;

    /* Should have been checked in resolve_ref_in_select_and_group(). */
    DBUG_ASSERT(*ref && (*ref)->fixed());
    /*
      Here, a subset of actions performed by Item_ref::set_properties
      is not enough. So we pass ptr to NULL into Item_[direct]_ref
      constructor, so no initialization is performed, and call 
      fix_fields() below.
    */
    save= *ref;
    *ref= NULL;                             // Don't call set_properties()
    rf= (place == IN_HAVING ?
         new (thd->mem_root)
         Item_ref(thd, context, ref, table_name,
                  field_name, alias_name_used) :
         (!select->group_list.elements ?
         new (thd->mem_root)
          Item_direct_ref(thd, context, ref, table_name,
                          field_name, alias_name_used) :
         new (thd->mem_root)
          Item_outer_ref(thd, context, ref, table_name,
                         field_name, alias_name_used)));
    *ref= save;
    if (!rf)
      return -1;

    if (place != IN_HAVING && select->group_list.elements)
    {
      outer_context->select_lex->inner_refs_list.push_back((Item_outer_ref*)rf,
                                                           thd->mem_root);
      ((Item_outer_ref*)rf)->in_sum_func= thd->lex->in_sum_func;
    }
    thd->change_item_tree(reference, rf);
    /*
      rf is Item_ref => never substitute other items (in this case)
      during fix_fields() => we can use rf after fix_fields()
    */
    DBUG_ASSERT(!rf->fixed());                // Assured by Item_ref()
    if (rf->fix_fields(thd, reference) || rf->check_cols(1))
      return -1;

    /*
      We can not "move" aggregate function in the place where
      its arguments are not defined.
    */
    set_max_sum_func_level(thd, select);
    mark_as_dependent(thd, last_checked_context->select_lex,
                      context->select_lex, rf,
                      rf, false);

    return 0;
  }
  else
  {
    /*
      We can not "move" aggregate function in the place where
      its arguments are not defined.
    */
    set_max_sum_func_level(thd, select);
    mark_as_dependent(thd, last_checked_context->select_lex,
                      context->select_lex,
                      this, (Item_ident*)*reference, false);
    if (last_checked_context->select_lex->having_fix_field)
    {
      Item_ref *rf;
      rf= new (thd->mem_root) Item_ref(thd, context,
                                       (*from_field)->table->s->db,
                                       Lex_cstring_strlen((*from_field)->
                                                          table->alias.c_ptr()),
                                       field_name);
      if (!rf)
        return -1;
      thd->change_item_tree(reference, rf);
      /*
        rf is Item_ref => never substitute other items (in this case)
        during fix_fields() => we can use rf after fix_fields()
      */
      DBUG_ASSERT(!rf->fixed());                // Assured by Item_ref()
      if (rf->fix_fields(thd, reference) || rf->check_cols(1))
        return -1;
      return 0;
    }
  }
  return 1;
}


/**
  Resolve the name of a column reference.

  The method resolves the column reference represented by 'this' as a column
  present in one of: FROM clause, SELECT clause, GROUP BY clause of a query
  Q, or in outer queries that contain Q.

  The name resolution algorithm used is (where [T_j] is an optional table
  name that qualifies the column name):

  @code
    resolve_column_reference([T_j].col_ref_i)
    {
      search for a column or derived column named col_ref_i
      [in table T_j] in the FROM clause of Q;

      if such a column is NOT found AND    // Lookup in outer queries.
         there are outer queries
      {
        for each outer query Q_k beginning from the inner-most one
        {
          search for a column or derived column named col_ref_i
          [in table T_j] in the FROM clause of Q_k;

          if such a column is not found
            Search for a column or derived column named col_ref_i
            [in table T_j] in the SELECT and GROUP clauses of Q_k.
        }
      }
    }
  @endcode

    Notice that compared to Item_ref::fix_fields, here we first search the FROM
    clause, and then we search the SELECT and GROUP BY clauses.

  @param[in]     thd        current thread
  @param[in,out] reference  view column if this item was resolved to a
    view column

  @retval
    TRUE  if error
  @retval
    FALSE on success
*/

bool Item_field::fix_fields(THD *thd, Item **reference)
{
  DBUG_ASSERT(fixed() == 0);
  Field *from_field= (Field *)not_found_field;
  bool outer_fixed= false;
  SELECT_LEX *select;
  if (context)
  {
    select= context->select_lex;
  }
  else
  {
    // No real name resolution, used somewhere in SP
    DBUG_ASSERT(field);
    select= NULL;
  }


  if (select && select->in_tvc)
  {
    my_error(ER_FIELD_REFERENCE_IN_TVC, MYF(0), full_name());
    return(1);
  }

  if (!field)					// If field is not checked
  {
    TABLE_LIST *table_list;
    /*
      In case of view, find_field_in_tables() write pointer to view field
      expression to 'reference', i.e. it substitute that expression instead
      of this Item_field
    */
    DBUG_ASSERT(context);
    if ((from_field= find_field_in_tables(thd, this,
                                          context->first_name_resolution_table,
                                          context->last_name_resolution_table,
                                          context->ignored_tables,
                                          reference,
                                          thd->lex->use_only_table_context ?
                                            REPORT_ALL_ERRORS : 
                                            IGNORE_EXCEPT_NON_UNIQUE,
                                          !any_privileges,
                                          TRUE)) ==
	not_found_field)
    {
      int ret;

      /* Look up in current select's item_list to find aliased fields */
      if (select && select->is_item_list_lookup)
      {
        uint counter;
        enum_resolution_type resolution;
        Item** res= find_item_in_list(this,
                                      select->item_list,
                                      &counter, REPORT_EXCEPT_NOT_FOUND,
                                      &resolution);
        if (!res)
          return 1;
        if (resolution == RESOLVED_AGAINST_ALIAS)
          alias_name_used= TRUE;
        if (res != (Item **)not_found_item)
        {
          if ((*res)->type() == Item::FIELD_ITEM)
          {
            /*
              It's an Item_field referencing another Item_field in the select
              list.
              Use the field from the Item_field in the select list and leave
              the Item_field instance in place.
            */

            Field *new_field= (*((Item_field**)res))->field;

            if (unlikely(new_field == NULL))
            {
              /* The column to which we link isn't valid. */
              my_error(ER_BAD_FIELD_ERROR, MYF(0), (*res)->name.str,
                       thd->where);
              return(1);
            }

            /*
              We can not "move" aggregate function in the place where
              its arguments are not defined.
            */
            set_max_sum_func_level(thd, select);
            set_field(new_field);
            depended_from= (*((Item_field**)res))->depended_from;
            return 0;
          }
          else
          {
            /*
              It's not an Item_field in the select list so we must make a new
              Item_ref to point to the Item in the select list and replace the
              Item_field created by the parser with the new Item_ref.
            */
            Item_ref *rf= new (thd->mem_root)
              Item_ref(thd, context, db_name, table_name, field_name);
            if (!rf)
              return 1;
            bool err= rf->fix_fields(thd, (Item **) &rf) || rf->check_cols(1);
            if (err)
              return TRUE;
           
            thd->change_item_tree(reference,
                                  select->context_analysis_place ==
                                  IN_GROUP_BY &&
                                  alias_name_used ? *rf->ref : rf);

            /*
              We can not "move" aggregate function in the place where
              its arguments are not defined.
            */
            set_max_sum_func_level(thd, select);
            return FALSE;
          }
        }
      }

      if (unlikely(!select))
      {
        my_error(ER_BAD_FIELD_ERROR, MYF(0), full_name(), thd->where);
        goto error;
      }
      if ((ret= fix_outer_field(thd, &from_field, reference)) < 0)
        goto error;
      outer_fixed= TRUE;
      if (!ret)
        goto mark_non_agg_field;
    }
    else if (!from_field)
      goto error;

    table_list= (cached_table ? cached_table :
                 from_field != view_ref_found ?
                 from_field->table->pos_in_table_list : 0);
    if (!outer_fixed && table_list && table_list->select_lex &&
        context->select_lex &&
        table_list->select_lex != context->select_lex &&
        !context->select_lex->is_merged_child_of(table_list->select_lex) &&
        is_outer_table(table_list, context->select_lex))
    {
      int ret;
      if ((ret= fix_outer_field(thd, &from_field, reference)) < 0)
        goto error;
      outer_fixed= 1;
      if (!ret)
        goto mark_non_agg_field;
    }

    if (!thd->lex->current_select->no_wrap_view_item &&
        thd->lex->in_sum_func &&
        thd->lex == select->parent_lex &&
        thd->lex->in_sum_func->nest_level == 
        select->nest_level)
      set_if_bigger(thd->lex->in_sum_func->max_arg_level,
                    select->nest_level);
    /*
      if it is not expression from merged VIEW we will set this field.

      We can leave expression substituted from view for next PS/SP rexecution
      (i.e. do not register this substitution for reverting on cleanup()
      (register_item_tree_changing())), because this subtree will be
      fix_field'ed during setup_tables()->setup_underlying() (i.e. before
      all other expressions of query, and references on tables which do
      not present in query will not make problems.

      Also we suppose that view can't be changed during PS/SP life.
    */
    if (from_field == view_ref_found)
      return FALSE;

    set_field(from_field);
  }
  else if (should_mark_column(thd->column_usage))
  {
    TABLE *table= field->table;
    MY_BITMAP *current_bitmap, *other_bitmap;
    if (thd->column_usage == MARK_COLUMNS_READ)
    {
      current_bitmap= table->read_set;
      other_bitmap=   table->write_set;
    }
    else
    {
      current_bitmap= table->write_set;
      other_bitmap=   table->read_set;
    }
    if (!bitmap_fast_test_and_set(current_bitmap, field->field_index))
    {
      if (!bitmap_is_set(other_bitmap, field->field_index))
      {
        /* First usage of column */
        table->used_fields++;                     // Used to optimize loops
        /* purecov: begin inspected */
        table->covering_keys.intersect(field->part_of_key);
        /* purecov: end */
      }
    }
  }
#ifndef NO_EMBEDDED_ACCESS_CHECKS
  if (any_privileges)
  {
    const char *db, *tab;
    db=  field->table->s->db.str;
    tab= field->table->s->table_name.str;
    if (!(have_privileges= (get_column_grant(thd, &field->table->grant,
                                             db, tab, field_name.str) &
                            VIEW_ANY_ACL)))
    {
      my_error(ER_COLUMNACCESS_DENIED_ERROR, MYF(0),
               "ANY", thd->security_ctx->priv_user,
               thd->security_ctx->host_or_ip, field_name.str, tab);
      goto error;
    }
  }
#endif
  base_flags|= item_base_t::FIXED;
  if (thd->variables.sql_mode & MODE_ONLY_FULL_GROUP_BY &&
      !outer_fixed && !thd->lex->in_sum_func &&
      select &&
      select->cur_pos_in_select_list != UNDEF_POS &&
      select->join)
  {
    select->join->non_agg_fields.push_back(this, thd->mem_root);
    marker= select->cur_pos_in_select_list;
  }
mark_non_agg_field:
  /*
    table->pos_in_table_list can be 0 when fixing partition functions
    or virtual fields.
  */
  if (fixed() && (thd->variables.sql_mode & MODE_ONLY_FULL_GROUP_BY) &&
      field->table->pos_in_table_list)
  {
    /*
      Mark selects according to presence of non aggregated fields.
      Fields from outer selects added to the aggregate function
      outer_fields list as it's unknown at the moment whether it's
      aggregated or not.
      We're using the select lex of the cached table (if present).
    */
    SELECT_LEX *select_lex;
    if (cached_table)
      select_lex= cached_table->select_lex;
    else if (!(select_lex= field->table->pos_in_table_list->select_lex))
    {
      /*
        This can only happen when there is no real table in the query.
        We are using the field's resolution context. context->select_lex is eee
        safe for use because it's either the SELECT we want to use 
        (the current level) or a stub added by non-SELECT queries.
      */
      select_lex= context->select_lex;
    }
    if (!thd->lex->in_sum_func)
      select_lex->set_non_agg_field_used(true);
    else
    {
      if (outer_fixed)
        thd->lex->in_sum_func->outer_fields.push_back(this, thd->mem_root);
      else if (thd->lex->in_sum_func->nest_level !=
          select->nest_level)
        select_lex->set_non_agg_field_used(true);
    }
  }
  return FALSE;

error:
  context->process_error(thd);
  return TRUE;
}

bool Item_field::post_fix_fields_part_expr_processor(void *int_arg)
{
  DBUG_ASSERT(fixed());
  if (field->vcol_info)
    field->vcol_info->mark_as_in_partitioning_expr();
  /*
    Update table_name to be real table name, not the alias. Because alias is
    reallocated for every statement, and this item has a long life time */
  table_name= field->table->s->table_name;
  return FALSE;
}

bool Item_field::check_valid_arguments_processor(void *bool_arg)
{
  Virtual_column_info *vcol= field->vcol_info;
  if (!vcol)
    return FALSE;
  return vcol->expr->walk(&Item::check_partition_func_processor, 0, NULL)
      || vcol->expr->walk(&Item::check_valid_arguments_processor, 0, NULL);
}

void Item_field::cleanup()
{
  DBUG_ENTER("Item_field::cleanup");
  Item_ident::cleanup();
  depended_from= NULL;
  /*
    Even if this object was created by direct link to field in setup_wild()
    it will be linked correctly next time by name of field and table alias.
    I.e. we can drop 'field'.
   */
  field= 0;
  item_equal= NULL;
  null_value= FALSE;
  refers_to_temp_table= FALSE;
  DBUG_VOID_RETURN;
}

/**
  Find a field among specified multiple equalities.

  The function first searches the field among multiple equalities
  of the current level (in the cond_equal->current_level list).
  If it fails, it continues searching in upper levels accessed
  through a pointer cond_equal->upper_levels.
  The search terminates as soon as a multiple equality containing 
  the field is found. 

  @param cond_equal   reference to list of multiple equalities where
                      the field (this object) is to be looked for

  @return
    - First Item_equal containing the field, if success
    - 0, otherwise
*/

Item_equal *Item_field::find_item_equal(COND_EQUAL *cond_equal)
{
  Item_equal *item= 0;
  while (cond_equal)
  {
    List_iterator_fast<Item_equal> li(cond_equal->current_level);
    while ((item= li++))
    {
      if (item->contains(field))
        return item;
    }
    /* 
      The field is not found in any of the multiple equalities
      of the current level. Look for it in upper levels
    */
    cond_equal= cond_equal->upper_levels;
  }
  return 0;
}


/**
  Set a pointer to the multiple equality the field reference belongs to
  (if any).

  The function looks for a multiple equality containing the field item
  among those referenced by arg.
  In the case such equality exists the function does the following.
  If the found multiple equality contains a constant, then the field
  reference is substituted for this constant, otherwise it sets a pointer
  to the multiple equality in the field item.


  @param arg    reference to list of multiple equalities where
                the field (this object) is to be looked for

  @note
    This function is supposed to be called as a callback parameter in calls
    of the compile method.

  @return
    - pointer to the replacing constant item, if the field item was substituted
    - pointer to the field item, otherwise.
*/

Item *Item_field::propagate_equal_fields(THD *thd,
                                         const Context &ctx,
                                         COND_EQUAL *arg)
{
  if (!(item_equal= find_item_equal(arg)))
    return this;
  if (!field->can_be_substituted_to_equal_item(ctx, item_equal))
  {
    item_equal= NULL;
    return this;
  }
  Item *item= item_equal->get_const();
  if (!item)
  {
    /*
      The found Item_equal is Okey, but it does not have a constant
      item yet. Keep this->item_equal point to the found Item_equal.
    */
    return this;
  }
  if (!(item= field->get_equal_const_item(thd, ctx, item)))
  {
    /*
      Could not do safe conversion from the original constant item
      to a field-compatible constant item.
      For example, we tried to optimize:
        WHERE date_column=' garbage ' AND LENGTH(date_column)=8;
      to
        WHERE date_column=' garbage ' AND LENGTH(DATE'XXXX-YY-ZZ');
      but failed to create a valid DATE literal from the given string literal.

      Do not do constant propagation in such cases and unlink
      "this" from the found Item_equal (as this equality not useful).
    */
    item_equal= NULL;
    return this;
  }
  return item;
}


/**
  Replace an Item_field for an equal Item_field that evaluated earlier
  (if any).

  If this->item_equal points to some item and coincides with arg then
  the function returns a pointer to an item that is taken from
  the very beginning of the item_equal list which the Item_field
  object refers to (belongs to) unless item_equal contains  a constant
  item. In this case the function returns this constant item, 
  (if the substitution does not require conversion).   
  If the Item_field object does not refer any Item_equal object
  'this' is returned .

  @param arg   NULL or points to so some item of the Item_equal type  


  @note
    This function is supposed to be called as a callback parameter in calls
    of the transformer method.

  @return
    - pointer to a replacement Item_field if there is a better equal item or
      a pointer to a constant equal item;
    - this - otherwise.
*/

Item *Item_field::replace_equal_field(THD *thd, uchar *arg)
{
  REPLACE_EQUAL_FIELD_ARG* param= (REPLACE_EQUAL_FIELD_ARG*)arg;
  if (item_equal && item_equal == param->item_equal)
  {
    Item *const_item2= item_equal->get_const();
    if (const_item2)
    {
      /*
        Currently we don't allow to create Item_equal with compare_type()
        different from its Item_field's cmp_type().
        Field_xxx::test_if_equality_guarantees_uniqueness() prevent this.
        Also, Item_field::propagate_equal_fields() does not allow to assign
        this->item_equal to any instances of Item_equal if "this" is used
        in a non-native comparison context, or with an incompatible collation.
        So the fact that we have (item_equal != NULL) means that the currently
        processed function (the owner of "this") uses the field in its native
        comparison context, and it's safe to replace it to the constant from
        item_equal.
      */
      DBUG_ASSERT(type_handler_for_comparison()->cmp_type() ==
                  item_equal->compare_type_handler()->cmp_type());
      return const_item2;
    }
    Item_ident *subst=
      (Item_ident *) (item_equal->get_first(param->context_tab, this));
    if (subst)
    {
      Item_field *subst2= (Item_field *) (subst->real_item());
      if (subst2 && !field->eq(subst2->field))
      return subst2;
    }
  }
  return this;
}


void Item::init_make_send_field(Send_field *tmp_field,
                                const Type_handler *h)
{
  tmp_field->db_name=		empty_clex_str;
  tmp_field->org_table_name=	empty_clex_str;
  tmp_field->org_col_name=	empty_clex_str;
  tmp_field->table_name=	empty_clex_str;
  tmp_field->col_name=	        name;
  tmp_field->flags=             (maybe_null() ? 0 : NOT_NULL_FLAG) | 
                                (my_binary_compare(charset_for_protocol()) ?
                                 BINARY_FLAG : 0);
  tmp_field->set_handler(h);
  tmp_field->length=max_length;
  tmp_field->decimals=decimals;
  if (unsigned_flag)
    tmp_field->flags |= UNSIGNED_FLAG;
  static_cast<Send_field_extended_metadata>(*tmp_field)=
    Send_field_extended_metadata();
  h->Item_append_extended_type_info(tmp_field, this);
}

void Item::make_send_field(THD *thd, Send_field *tmp_field)
{
  init_make_send_field(tmp_field, type_handler());
}


void Item_empty_string::make_send_field(THD *thd, Send_field *tmp_field)
{
  init_make_send_field(tmp_field, string_type_handler());
}


/**
  Verifies that the input string is well-formed according to its character set.
  @param send_error   If true, call my_error if string is not well-formed.

  Will truncate input string if it is not well-formed.

  @return
  If well-formed: input string.
  If not well-formed:
    if strict mode: NULL pointer and we set this Item's value to NULL
    if not strict mode: input string truncated up to last good character
 */
String *Item::check_well_formed_result(String *str, bool send_error)
{
  /* Check whether we got a well-formed string */
  CHARSET_INFO *cs= str->charset();
  uint wlen= str->well_formed_length();
  null_value= false;
  if (unlikely(wlen < str->length()))
  {
    THD *thd= current_thd;
    char hexbuf[7];
    uint diff= str->length() - wlen;
    set_if_smaller(diff, 3);
    octet2hex(hexbuf, str->ptr() + wlen, diff);
    if (send_error)
    {
      my_error(ER_INVALID_CHARACTER_STRING, MYF(0),
               cs->cs_name.str,  hexbuf);
      return 0;
    }
    if (thd->is_strict_mode())
    {
      null_value= 1;
      str= 0;
    }
    else
    {
      str->length(wlen);
    }
    push_warning_printf(thd, Sql_condition::WARN_LEVEL_WARN,
                        ER_INVALID_CHARACTER_STRING,
                        ER_THD(thd, ER_INVALID_CHARACTER_STRING), cs->cs_name.str,
                        hexbuf);
  }
  return str;
}


/**
  Copy a string with optional character set conversion.
*/
bool
String_copier_for_item::copy_with_warn(CHARSET_INFO *dstcs, String *dst,
                                       CHARSET_INFO *srccs, const char *src,
                                       uint32 src_length, uint32 nchars)
{
  if (unlikely((dst->copy(dstcs, srccs, src, src_length, nchars, this))))
    return true; // EOM
  const char *pos;
  if (unlikely(pos= well_formed_error_pos()))
  {
    ErrConvString err(pos, src_length - (pos - src), &my_charset_bin);
    push_warning_printf(m_thd, Sql_condition::WARN_LEVEL_WARN,
                        ER_INVALID_CHARACTER_STRING,
                        ER_THD(m_thd, ER_INVALID_CHARACTER_STRING),
                        srccs == &my_charset_bin ?
                        dstcs->cs_name.str : srccs->cs_name.str,
                        err.ptr());
    return false;
  }
  if (unlikely(pos= cannot_convert_error_pos()))
  {
    char buf[16];
    int mblen= srccs->charlen(pos, src + src_length);
    DBUG_ASSERT(mblen > 0 && mblen * 2 + 1 <= (int) sizeof(buf));
    octet2hex(buf, pos, mblen);
    push_warning_printf(m_thd, Sql_condition::WARN_LEVEL_WARN,
                        ER_CANNOT_CONVERT_CHARACTER,
                        ER_THD(m_thd, ER_CANNOT_CONVERT_CHARACTER),
                        srccs->cs_name.str, buf, dstcs->cs_name.str);
    return false;
  }
  return false;
}


/*
  Compare two items using a given collation
  
  SYNOPSIS
    eq_by_collation()
    item               item to compare with
    binary_cmp         TRUE <-> compare as binaries
    cs                 collation to use when comparing strings

  DESCRIPTION
    This method works exactly as Item::eq if the collation cs coincides with
    the collation of the compared objects. Otherwise, first the collations that
    differ from cs are replaced for cs and then the items are compared by
    Item::eq. After the comparison the original collations of items are
    restored.

  RETURN
    1    compared items has been detected as equal   
    0    otherwise
*/

bool Item::eq_by_collation(Item *item, bool binary_cmp, CHARSET_INFO *cs)
{
  CHARSET_INFO *save_cs= 0;
  CHARSET_INFO *save_item_cs= 0;
  if (collation.collation != cs)
  {
    save_cs= collation.collation;
    collation.collation= cs;
  }
  if (item->collation.collation != cs)
  {
    save_item_cs= item->collation.collation;
    item->collation.collation= cs;
  }
  bool res= eq(item, binary_cmp);
  if (save_cs)
    collation.collation= save_cs;
  if (save_item_cs)
    item->collation.collation= save_item_cs;
  return res;
}  


/* ARGSUSED */
void Item_field::make_send_field(THD *thd, Send_field *tmp_field)
{
  field->make_send_field(tmp_field);
  DBUG_ASSERT(tmp_field->table_name.str != 0);
  if (name.str)
  {
    DBUG_ASSERT(name.length == strlen(name.str));
    tmp_field->col_name= name;		// Use user supplied name
  }
  if (table_name.str)
    tmp_field->table_name= table_name;
  if (db_name.str)
    tmp_field->db_name= db_name;
}


/**
  Save a field value in another field

  @param from             Field to take the value from
  @param [out] null_value Pointer to the null_value flag to set
  @param to               Field to save the value in
  @param no_conversions   How to deal with NULL value

  @details
  The function takes the value of the field 'from' and, if this value
  is not null, it saves in the field 'to' setting off the flag referenced
  by 'null_value'. Otherwise this flag is set on and field 'to' is
  also set to null possibly with conversion.

  @note
  This function is used by the functions Item_field::save_in_field,
  Item_field::save_org_in_field and Item_ref::save_in_field

  @retval FALSE OK
  @retval TRUE  Error

*/

static int save_field_in_field(Field *from, bool *null_value,
                               Field *to, bool no_conversions)
{
  int res;
  DBUG_ENTER("save_field_in_field");
  if (from->is_null())
  {
    (*null_value)= 1;
    DBUG_RETURN(set_field_to_null_with_conversions(to, no_conversions));
  }
  to->set_notnull();
  (*null_value)= 0;

  /*
    If we're setting the same field as the one we're reading from there's 
    nothing to do. This can happen in 'SET x = x' type of scenarios.
  */
  if (to == from)
    DBUG_RETURN(0);

  res= field_conv(to, from);
  DBUG_RETURN(res);
}


fast_field_copier Item_field::setup_fast_field_copier(Field *to)
{
  return to->get_fast_field_copier(field);
}

void Item_field::save_in_result_field(bool no_conversions)
{
  bool unused;
  save_field_in_field(field, &unused, result_field, no_conversions);
}

/**
  Set a field's value from a item.
*/

void Item_field::save_org_in_field(Field *to,
                                   fast_field_copier fast_field_copier_func)
{
  DBUG_ENTER("Item_field::save_org_in_field");
  DBUG_PRINT("enter", ("setup: %p  data: %p",
                       to, fast_field_copier_func));
  if (fast_field_copier_func)
  {
    if (field->is_null())
    {
      null_value= TRUE;
      set_field_to_null_with_conversions(to, TRUE);
      DBUG_VOID_RETURN;
    }
    to->set_notnull();
    if (to == field)
    {
      null_value= 0;
      DBUG_VOID_RETURN;
    }
    (*fast_field_copier_func)(to, field);
  }
  else
    save_field_in_field(field, &null_value, to, TRUE);
  DBUG_VOID_RETURN;
}


int Item_field::save_in_field(Field *to, bool no_conversions)
{
  return save_field_in_field(result_field, &null_value, to, no_conversions);
}


/**
  Store null in field.

  This is used on INSERT.
  Allow NULL to be inserted in timestamp and auto_increment values.

  @param field		Field where we want to store NULL

  @retval
    0   ok
  @retval
    1   Field doesn't support NULL values and can't handle 'field = NULL'
*/

int Item_null::save_in_field(Field *field, bool no_conversions)
{
  return set_field_to_null_with_conversions(field, no_conversions);
}


/**
  Store null in field.

  @param field		Field where we want to store NULL

  @retval
    0	 OK
  @retval
    1	 Field doesn't support NULL values
*/

int Item_null::save_safe_in_field(Field *field)
{
  return set_field_to_null(field);
}


/*
  This implementation can lose str_value content, so if the
  Item uses str_value to store something, it should
  reimplement it's ::save_in_field() as Item_string, for example, does.

  Note: all Item_XXX::val_str(str) methods must NOT assume that
  str != str_value. For example, see fix for bug #44743.
*/
int Item::save_str_in_field(Field *field, bool no_conversions)
{
  String *result;
  CHARSET_INFO *cs= collation.collation;
  char buff[MAX_FIELD_WIDTH];		// Alloc buffer for small columns
  str_value.set_buffer_if_not_allocated(buff, sizeof(buff), cs);
  result=val_str(&str_value);
  if (null_value)
  {
    str_value.set_buffer_if_not_allocated(0, 0, cs);
    return set_field_to_null_with_conversions(field, no_conversions);
  }

  /* NOTE: If null_value == FALSE, "result" must be not NULL.  */

  field->set_notnull();
  int error= field->store(result->ptr(),result->length(),cs);
  str_value.set_buffer_if_not_allocated(0, 0, cs);
  return error;
}


int Item::save_real_in_field(Field *field, bool no_conversions)
{
  double nr= val_real();
  if (null_value)
    return set_field_to_null_with_conversions(field, no_conversions);
  field->set_notnull();
  return field->store(nr);
}


int Item::save_decimal_in_field(Field *field, bool no_conversions)
{
  VDec value(this);
  if (value.is_null())
    return set_field_to_null_with_conversions(field, no_conversions);
  field->set_notnull();
  return field->store_decimal(value.ptr());
}


int Item::save_int_in_field(Field *field, bool no_conversions)
{
  longlong nr= val_int();
  if (null_value)
    return set_field_to_null_with_conversions(field, no_conversions);
  field->set_notnull();
  return field->store(nr, unsigned_flag);
}


int Item::save_in_field(Field *field, bool no_conversions)
{
  int error= type_handler()->Item_save_in_field(this, field, no_conversions);
  return error ? error : (field->table->in_use->is_error() ? 1 : 0);
}


bool Item::save_in_param(THD *thd, Item_param *param)
{
  return param->set_from_item(thd, this);
}


int Item_string::save_in_field(Field *field, bool no_conversions)
{
  String *result;
  result=val_str(&str_value);
  return save_str_value_in_field(field, result);
}


Item *Item_string::clone_item(THD *thd)
{
  LEX_CSTRING val;
  str_value.get_value(&val);
  return new (thd->mem_root) Item_string(thd, name, val, collation.collation);
}


Item_basic_constant *
Item_string::make_string_literal_concat(THD *thd, const LEX_CSTRING *str)
{
  append(str->str, (uint32) str->length);
  if (!(collation.repertoire & MY_REPERTOIRE_EXTENDED))
  {
    // If the string has been pure ASCII so far, check the new part.
    CHARSET_INFO *cs= thd->variables.collation_connection;
    collation.repertoire|= my_string_repertoire(cs, str->str, str->length);
  }
  return this;
}


/*
  If "this" is a reasonably short pure ASCII string literal,
  try to parse known ODBC-style date, time or timestamp literals,
  e.g:
  SELECT {d'2001-01-01'};
  SELECT {t'10:20:30'};
  SELECT {ts'2001-01-01 10:20:30'};
*/
Item *Item_string::make_odbc_literal(THD *thd, const LEX_CSTRING *typestr)
{
  Item_literal *res;
  const Type_handler *h;
  if (collation.repertoire == MY_REPERTOIRE_ASCII &&
      str_value.length() < MAX_DATE_STRING_REP_LENGTH * 4 &&
      (h= Type_handler::odbc_literal_type_handler(typestr)) &&
      (res= h->create_literal_item(thd, val_str(NULL), false)))
    return res;
  /*
    h->create_literal_item() returns NULL if failed to parse the string,
    or the string format did not match the type, e.g.:  {d'2001-01-01 10:10:10'}
  */
  return this;
}


static int save_int_value_in_field (Field *field, longlong nr,
                                    bool null_value, bool unsigned_flag)
{
  if (null_value)
    return set_field_to_null(field);
  field->set_notnull();
  return field->store(nr, unsigned_flag);
}


int Item_int::save_in_field(Field *field, bool no_conversions)
{
  return save_int_value_in_field (field, val_int(), null_value, unsigned_flag);
}


Item *Item_int::clone_item(THD *thd)
{
  return new (thd->mem_root) Item_int(thd, name.str, value, max_length, unsigned_flag);
}


void Item_datetime::set(longlong packed, enum_mysql_timestamp_type ts_type)
{
  unpack_time(packed, &ltime, ts_type);
}

int Item_datetime::save_in_field(Field *field, bool no_conversions)
{
  field->set_notnull();
  return field->store_time_dec(&ltime, decimals);
}

longlong Item_datetime::val_int()
{
  return TIME_to_ulonglong(&ltime);
}

int Item_decimal::save_in_field(Field *field, bool no_conversions)
{
  field->set_notnull();
  return field->store_decimal(&decimal_value);
}


Item *Item_int_with_ref::clone_item(THD *thd)
{
  DBUG_ASSERT(ref->const_item());
  /*
    We need to evaluate the constant to make sure it works with
    parameter markers.
  */
  return (ref->unsigned_flag ?
          new (thd->mem_root)
          Item_uint(thd, ref->name.str, ref->val_int(), ref->max_length) :
          new (thd->mem_root)
          Item_int(thd, ref->name.str, ref->val_int(), ref->max_length));
}


Item *Item::neg(THD *thd)
{
  return new (thd->mem_root) Item_func_neg(thd, this);
}

Item *Item_int::neg(THD *thd)
{
  /*
    The following if should never be true with code generated by
    our parser as LONGLONG_MIN values will be stored as decimal.
    The code is here in case someone generates an int from inside
    MariaDB
  */
  if (unlikely(value == LONGLONG_MIN))
  {
    /* Precision for int not big enough; Convert constant to decimal */
    Item_decimal *item= new (thd->mem_root) Item_decimal(thd, value, 0);
    return item ? item->neg(thd) : item;
  }
  if (value > 0)
    max_length++;
  else if (value < 0 && max_length)
    max_length--;
  value= -value;
  name= null_clex_str;
  return this;
}

Item *Item_decimal::neg(THD *thd)
{
  my_decimal_neg(&decimal_value);
  unsigned_flag= 0;
  name= null_clex_str;
  max_length= my_decimal_precision_to_length_no_truncation(
                      decimal_value.intg + decimals, decimals, unsigned_flag);
  return this;
}

Item *Item_float::neg(THD *thd)
{
  if (value > 0)
    max_length++;
  else if (value < 0 && max_length)
    max_length--;
  value= -value;
  presentation= 0;
  name= null_clex_str;
  return this;
}

Item *Item_uint::neg(THD *thd)
{
  Item_decimal *item;
  if (((ulonglong)value) <= LONGLONG_MAX)
    return new (thd->mem_root) Item_int(thd, -value, max_length+1);
  if (value == LONGLONG_MIN)
    return new (thd->mem_root) Item_int(thd, value, max_length+1);
  if (!(item= new (thd->mem_root) Item_decimal(thd, value, 1)))
    return 0;
  return item->neg(thd);
}


Item *Item_uint::clone_item(THD *thd)
{
  return new (thd->mem_root) Item_uint(thd, name.str, value, max_length);
}

static uint nr_of_decimals(const char *str, const char *end)
{
  const char *decimal_point;

  /* Find position for '.' */
  for (;;)
  {
    if (str == end)
      return 0;
    if (*str == 'e' || *str == 'E')
      return NOT_FIXED_DEC;    
    if (*str++ == '.')
      break;
  }
  decimal_point= str;
  for ( ; str < end && my_isdigit(system_charset_info, *str) ; str++)
    ;
  if (str < end && (*str == 'e' || *str == 'E'))
    return NOT_FIXED_DEC;
  /*
    QQ:
    The number of decimal digist in fact should be (str - decimal_point - 1).
    But it seems the result of nr_of_decimals() is never used!

    In case of 'e' and 'E' nr_of_decimals returns NOT_FIXED_DEC.
    In case if there is no 'e' or 'E' parser code in sql_yacc.yy
    never calls Item_float::Item_float() - it creates Item_decimal instead.

    The only piece of code where we call Item_float::Item_float(str, len)
    without having 'e' or 'E' is item_xmlfunc.cc, but this Item_float
    never appears in metadata itself. Changing the code to return
    (str - decimal_point - 1) does not make any changes in the test results.

    This should be addressed somehow.
    Looks like a reminder from before real DECIMAL times.
  */
  return (uint) (str - decimal_point);
}


/**
  This function is only called during parsing:
  - when parsing SQL query from sql_yacc.yy
  - when parsing XPath query from item_xmlfunc.cc
  We will signal an error if value is not a true double value (overflow):
  eng: Illegal %s '%-.192s' value found during parsing
  
  Note: the string is NOT null terminated when called from item_xmlfunc.cc,
  so this->name will contain some SQL query tail behind the "length" bytes.
  This is Ok for now, as this Item is never seen in SHOW,
  or EXPLAIN, or anywhere else in metadata.
  Item->name should be fixed to use LEX_STRING eventually.
*/

Item_float::Item_float(THD *thd, const char *str_arg, size_t length):
  Item_num(thd)
{
  int error;
  char *end_not_used;
  value= my_charset_bin.strntod((char*) str_arg, length, &end_not_used, &error);
  if (unlikely(error))
  {
    char tmp[NAME_LEN + 2];
    my_snprintf(tmp, sizeof(tmp), "%.*s", static_cast<int>(length), str_arg);
    my_error(ER_ILLEGAL_VALUE_FOR_TYPE, MYF(0), "double", tmp);
  }
  presentation= name.str= str_arg;
  name.length= strlen(str_arg);
  decimals=(uint8) nr_of_decimals(str_arg, str_arg+length);
  max_length=(uint32)length;
}


int Item_float::save_in_field(Field *field, bool no_conversions)
{
  double nr= val_real();
  if (null_value)
    return set_field_to_null(field);
  field->set_notnull();
  return field->store(nr);
}


void Item_float::print(String *str, enum_query_type query_type)
{
  if (presentation)
  {
    str->append(presentation, strlen(presentation));
    return;
  }
  char buffer[20];
  String num(buffer, sizeof(buffer), &my_charset_bin);
  num.set_real(value, decimals, &my_charset_bin);
  str->append(num);
}


inline uint char_val(char X)
{
  return (uint) (X >= '0' && X <= '9' ? X-'0' :
		 X >= 'A' && X <= 'Z' ? X-'A'+10 :
		 X-'a'+10);
}


void Item_hex_constant::hex_string_init(THD *thd, const char *str, size_t str_length)
{
  max_length=(uint)((str_length+1)/2);
  char *ptr=(char*) thd->alloc(max_length+1);
  if (!ptr)
  {
    str_value.set("", 0, &my_charset_bin);
    return;
  }
  str_value.set(ptr,max_length,&my_charset_bin);
  char *end=ptr+max_length;
  if (max_length*2 != str_length)
    *ptr++=char_val(*str++);			// Not even, assume 0 prefix
  while (ptr != end)
  {
    *ptr++= (char) (char_val(str[0])*16+char_val(str[1]));
    str+=2;
  }
  *ptr=0;					// Keep purify happy
  collation.set(&my_charset_bin, DERIVATION_COERCIBLE);
  unsigned_flag= 1;
}


void Item_hex_hybrid::print(String *str, enum_query_type query_type)
{
  uint32 len= MY_MIN(str_value.length(), sizeof(longlong));
  const char *ptr= str_value.ptr() + str_value.length() - len;
  str->append("0x",2);
  str->append_hex(ptr, len);
}


decimal_digits_t Item_hex_hybrid::decimal_precision() const
{
  switch (max_length) {// HEX                                 DEC
  case 0:              // ----                                ---
  case 1: return 3;    // 0xFF                                255
  case 2: return 5;    // 0xFFFF                            65535
  case 3: return 8;    // 0xFFFFFF                       16777215
  case 4: return 10;   // 0xFFFFFFFF                   4294967295
  case 5: return 13;   // 0xFFFFFFFFFF              1099511627775
  case 6: return 15;   // 0xFFFFFFFFFFFF          281474976710655
  case 7: return 17;   // 0xFFFFFFFFFFFFFF      72057594037927935
  }
  return 20;           // 0xFFFFFFFFFFFFFFFF 18446744073709551615
}


void Item_hex_string::print(String *str, enum_query_type query_type)
{
  str->append("X'",2);
  str->append_hex(str_value.ptr(), str_value.length());
  str->append('\'');
}


/*
  bin item.
  In string context this is a binary string.
  In number context this is a longlong value.
*/
  
Item_bin_string::Item_bin_string(THD *thd, const char *str, size_t str_length):
  Item_hex_hybrid(thd)
{
  const char *end= str + str_length - 1;
  char *ptr;
  uchar bits= 0;
  uint power= 1;

  max_length= (uint)((str_length + 7) >> 3);
  if (!(ptr= (char*) thd->alloc(max_length + 1)))
    return;
  str_value.set(ptr, max_length, &my_charset_bin);

  if (max_length > 0)
  {
    ptr+= max_length - 1;
    ptr[1]= 0;                     // Set end null for string
    for (; end >= str; end--)
    {
      if (power == 256)
      {
        power= 1;
        *ptr--= bits;
        bits= 0;
      }
      if (*end == '1')
        bits|= power;
      power<<= 1;
    }
    *ptr= (char) bits;
  }
  else
    ptr[0]= 0;

  collation.set(&my_charset_bin, DERIVATION_COERCIBLE);
}


void Item_date_literal::print(String *str, enum_query_type query_type)
{
  str->append(STRING_WITH_LEN("DATE'"));
  char buf[MAX_DATE_STRING_REP_LENGTH];
  int length= my_date_to_str(cached_time.get_mysql_time(), buf);
  str->append(buf, length);
  str->append('\'');
}


Item *Item_date_literal::clone_item(THD *thd)
{
  return new (thd->mem_root) Item_date_literal(thd, &cached_time);
}


bool Item_date_literal::get_date(THD *thd, MYSQL_TIME *ltime, date_mode_t fuzzydate)
{
  fuzzydate |= sql_mode_for_dates(thd);
  cached_time.copy_to_mysql_time(ltime);
  return (null_value= check_date_with_warn(thd, ltime, fuzzydate,
                                           MYSQL_TIMESTAMP_ERROR));
}


void Item_datetime_literal::print(String *str, enum_query_type query_type)
{
  str->append(STRING_WITH_LEN("TIMESTAMP'"));
  char buf[MAX_DATE_STRING_REP_LENGTH];
  int length= my_datetime_to_str(cached_time.get_mysql_time(), buf, decimals);
  str->append(buf, length);
  str->append('\'');
}


Item *Item_datetime_literal::clone_item(THD *thd)
{
  return new (thd->mem_root) Item_datetime_literal(thd, &cached_time, decimals);
}


bool Item_datetime_literal::get_date(THD *thd, MYSQL_TIME *ltime, date_mode_t fuzzydate)
{
  fuzzydate |= sql_mode_for_dates(thd);
  cached_time.copy_to_mysql_time(ltime);
  return (null_value= check_date_with_warn(thd, ltime, fuzzydate,
                                           MYSQL_TIMESTAMP_ERROR));
}


void Item_time_literal::print(String *str, enum_query_type query_type)
{
  str->append(STRING_WITH_LEN("TIME'"));
  char buf[MAX_DATE_STRING_REP_LENGTH];
  int length= my_time_to_str(cached_time.get_mysql_time(), buf, decimals);
  str->append(buf, length);
  str->append('\'');
}


Item *Item_time_literal::clone_item(THD *thd)
{
  return new (thd->mem_root) Item_time_literal(thd, &cached_time, decimals);
}


bool Item_time_literal::get_date(THD *thd, MYSQL_TIME *ltime, date_mode_t fuzzydate)
{
  cached_time.copy_to_mysql_time(ltime);
  if (fuzzydate & TIME_TIME_ONLY)
    return (null_value= false);
  return (null_value= check_date_with_warn(thd, ltime, fuzzydate,
                                           MYSQL_TIMESTAMP_ERROR));
}



/**
  Pack data in buffer for sending.
*/

bool Item_null::send(Protocol *protocol, st_value *buffer)
{
  return protocol->store_null();
}


/**
  Check if an item is a constant one and can be cached.

  @param arg [out] TRUE <=> Cache this item.

  @return TRUE  Go deeper in item tree.
  @return FALSE Don't go deeper in item tree.
*/

bool Item::cache_const_expr_analyzer(uchar **arg)
{
  bool *cache_flag= (bool*)*arg;
  if (!*cache_flag)
  {
    Item *item= real_item();
    /*
      Cache constant items unless it's a basic constant, constant field or
      a subselect (they use their own cache).
    */
    if (const_item() &&
        !(basic_const_item() || item->basic_const_item() ||
          item->type() == Item::NULL_ITEM || /* Item_name_const hack */
          item->type() == Item::FIELD_ITEM ||
          item->type() == SUBSELECT_ITEM ||
          item->type() == CACHE_ITEM ||
           /*
             Do not cache GET_USER_VAR() function as its const_item() may
             return TRUE for the current thread but it still may change
             during the execution.
           */
          (item->type() == Item::FUNC_ITEM &&
           ((Item_func*)item)->functype() == Item_func::GUSERVAR_FUNC)))
      *cache_flag= TRUE;
    return TRUE;
  }
  return FALSE;
}


/**
  Cache item if needed.

  @param arg   TRUE <=> Cache this item.

  @return cache if cache needed.
  @return this otherwise.
*/

Item* Item::cache_const_expr_transformer(THD *thd, uchar *arg)
{
  if (*(bool*)arg)
  {
    *((bool*)arg)= FALSE;
    Item_cache *cache= get_cache(thd);
    if (!cache)
      return NULL;
    cache->setup(thd, this);
    cache->store(this);
    return cache;
  }
  return this;
}

/**
  Find Item by reference in the expression
*/
bool Item::find_item_processor(void *arg)
{
  return (this == ((Item *) arg));
}

bool Item_field::send(Protocol *protocol, st_value *buffer)
{
  return protocol->store(result_field);
}


Item* Item::propagate_equal_fields_and_change_item_tree(THD *thd,
                                                        const Context &ctx,
                                                        COND_EQUAL *cond,
                                                        Item **place)
{
  Item *item= propagate_equal_fields(thd, ctx, cond);
  if (item && item != this)
    thd->change_item_tree(place, item);
  return item;
}


void Item_field::update_null_value() 
{ 
  /* 
    need to set no_errors to prevent warnings about type conversion 
    popping up.
  */
  THD *thd= field->table->in_use;
  int no_errors;

  no_errors= thd->no_errors;
  thd->no_errors= 1;
  type_handler()->Item_update_null_value(this);
  thd->no_errors= no_errors;
}


/*
  Add the field to the select list and substitute it for the reference to
  the field.

  SYNOPSIS
    Item_field::update_value_transformer()
    select_arg      current select

  DESCRIPTION
    If the field doesn't belong to the table being inserted into then it is
    added to the select list, pointer to it is stored in the ref_pointer_array
    of the select and the field itself is substituted for the Item_ref object.
    This is done in order to get correct values from update fields that
    belongs to the SELECT part in the INSERT .. SELECT .. ON DUPLICATE KEY
    UPDATE statement.

  RETURN
    0             if error occurred
    ref           if all conditions are met
    this field    otherwise
*/

Item *Item_field::update_value_transformer(THD *thd, uchar *select_arg)
{
  SELECT_LEX *select= (SELECT_LEX*)select_arg;
  DBUG_ASSERT(fixed());

  if (field->table != select->context.table_list->table &&
      type() != Item::TRIGGER_FIELD_ITEM)
  {
    List<Item> *all_fields= &select->join->all_fields;
    Ref_ptr_array &ref_pointer_array= select->ref_pointer_array;
    int el= all_fields->elements;
    Item_ref *ref;

    ref_pointer_array[el]= (Item*)this;
    all_fields->push_front((Item*)this, thd->mem_root);
    ref= new (thd->mem_root)
      Item_ref(thd, &select->context, &ref_pointer_array[el],
               table_name, field_name);
    return ref;
  }
  return this;
}


/**
  @brief
    Prepare AND/OR formula for extraction of a pushable condition

  @param checker  the checker callback function to be applied to the nodes
                  of the tree of the object
  @param arg      parameter to be passed to the checker

  @details
    This method recursively traverses this AND/OR condition and for each
    subformula of the condition it checks whether it can be usable for the
    extraction of a pushable condition. The criteria of pushability of
    a subformula is checked by the callback function 'checker' with one
    parameter arg. The subformulas that are not usable are marked with
    the flag MARKER_NO_EXTRACTION.
  @note
    This method is called before any call of build_pushable_cond.
    The flag MARKER_NO_EXTRACTION set in a subformula allows to avoid building
    clones for the subformulas that are not used in the pushable condition.
  @note
    This method is called for pushdown conditions into materialized
    derived tables/views optimization.
    Item::pushable_cond_checker_for_derived() is passed as the actual callback
    function.
    Also it is called for pushdown conditions in materialized IN subqueries.
    Item::pushable_cond_checker_for_subquery is passed as the actual
    callback function.
*/

void Item::check_pushable_cond(Pushdown_checker checker, uchar *arg)
{
  clear_extraction_flag();
  if (type() == Item::COND_ITEM)
  {
    bool and_cond= ((Item_cond*) this)->functype() == Item_func::COND_AND_FUNC;
    List_iterator<Item> li(*((Item_cond*) this)->argument_list());
    uint count= 0;
    Item *item;
    while ((item=li++))
    {
      item->check_pushable_cond(checker, arg);
      if (item->get_extraction_flag() !=  MARKER_NO_EXTRACTION)
        count++;
      else if (!and_cond)
        break;
    }
    if ((and_cond && count == 0) || item)
    {
      set_extraction_flag(MARKER_NO_EXTRACTION);
      if (and_cond)
        li.rewind();
      while ((item= li++))
        item->clear_extraction_flag();
    }
  }
  else if (!((this->*checker) (arg)))
    set_extraction_flag(MARKER_NO_EXTRACTION);
}


/**
  @brief
    Build condition extractable from this condition for pushdown

  @param thd      the thread handle
  @param checker  the checker callback function to be applied to the nodes
                  of the tree of the object to check if multiple equality
                  elements can be used to create equalities
  @param arg      parameter to be passed to the checker

  @details
    This method finds out what condition that can be pushed down can be
    extracted from this condition. If such condition C exists the
    method builds the item for it. The method uses the flag MARKER_NO_EXTRACTION
    set by the preliminary call of the method check_pushable_cond() to figure
    out whether a subformula is pushable or not.
    In the case when this item is a multiple equality a checker method is
    called to find the equal fields to build a new equality that can be
    pushed down.
  @note
    The built condition C is always implied by the condition cond
    (cond => C). The method tries to build the most restrictive such
    condition (i.e. for any other condition C' such that cond => C'
    we have C => C').
  @note
    The build item is not ready for usage: substitution for the field items
    has to be done and it has to be re-fixed.
  @note
    This method is called for pushdown conditions into materialized
    derived tables/views optimization.
    Item::pushable_equality_checker_for_derived() is passed as the actual
    callback function.
    Also it is called for pushdown conditions into materialized IN subqueries.
    Item::pushable_equality_checker_for_subquery() is passed as the actual
    callback function.

 @retval
    the built condition pushable into if such a condition exists
    NULL if there is no such a condition
*/

Item *Item::build_pushable_cond(THD *thd,
                                Pushdown_checker checker,
                                uchar *arg)
{
  bool is_multiple_equality= type() == Item::FUNC_ITEM &&
  ((Item_func*) this)->functype() == Item_func::MULT_EQUAL_FUNC;

  if (get_extraction_flag() == MARKER_NO_EXTRACTION)
    return 0;

  if (type() == Item::COND_ITEM)
  {
    bool cond_and= false;
    Item_cond *new_cond;
    if (((Item_cond*) this)->functype() == Item_func::COND_AND_FUNC)
    {
      cond_and= true;
      new_cond= new (thd->mem_root) Item_cond_and(thd);
    }
    else
      new_cond= new (thd->mem_root) Item_cond_or(thd);
    if (!new_cond)
      return 0;
    List_iterator<Item> li(*((Item_cond*) this)->argument_list());
    Item *item;
    bool is_fix_needed= false;

    while ((item=li++))
    {
      if (item->get_extraction_flag() == MARKER_NO_EXTRACTION)
      {
        if (!cond_and)
          return 0;
        continue;
      }
      Item *fix= item->build_pushable_cond(thd, checker, arg);
      if (!fix && !cond_and)
        return 0;
      if (!fix)
        continue;

      if (fix->type() == Item::COND_ITEM &&
          ((Item_cond*) fix)->functype() == Item_func::COND_AND_FUNC)
        is_fix_needed= true;

      if (new_cond->argument_list()->push_back(fix, thd->mem_root))
        return 0;
    }
    if (is_fix_needed && new_cond->fix_fields(thd, 0))
      return 0;

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
  else if (is_multiple_equality)
  {
    List<Item> equalities;
    Item *new_cond= NULL;
    if (((Item_equal *)this)->create_pushable_equalities(thd, &equalities,
                                                         checker, arg, true) ||
        (equalities.elements == 0))
      return 0;

    switch (equalities.elements)
    {
    case 0:
      return 0;
    case 1:
      new_cond= equalities.head();
      break;
    default:
      new_cond= new (thd->mem_root) Item_cond_and(thd, equalities);
      break;
    }
    if (new_cond && new_cond->fix_fields(thd, &new_cond))
      return 0;
    return new_cond;
  }
  else if (get_extraction_flag() != MARKER_NO_EXTRACTION)
    return build_clone(thd);
  return 0;
}


static
Item *get_field_item_for_having(THD *thd, Item *item, st_select_lex *sel)
{
  DBUG_ASSERT(item->type() == Item::FIELD_ITEM ||
              (item->type() == Item::REF_ITEM &&
               ((Item_ref *) item)->ref_type() == Item_ref::VIEW_REF)); 
  Item_field *field_item= NULL;
  table_map map= sel->master_unit()->derived->table->map;
  Item_equal *item_equal= item->get_item_equal();
  if (!item_equal)
    field_item= (Item_field *)(item->real_item());
  else
  {
    Item_equal_fields_iterator li(*item_equal);
    Item *equal_item;
    while ((equal_item= li++))
    {
      if (equal_item->used_tables() == map)
      {
        field_item= (Item_field *)(equal_item->real_item());
        break;
      }
    }
  }
  if (field_item)
  {
    Item_ref *ref= new (thd->mem_root) Item_ref(thd, &sel->context,
                                                field_item->field_name);
    return ref;
  }
  DBUG_ASSERT(0);
  return NULL; 
}


Item *Item_field::derived_field_transformer_for_having(THD *thd, uchar *arg)
{
  st_select_lex *sel= (st_select_lex *)arg;
  table_map tab_map= sel->master_unit()->derived->table->map;
  if (item_equal && !(item_equal->used_tables() & tab_map))
    return this;
  if (!item_equal && used_tables() != tab_map)
    return this;
  Item *item= get_field_item_for_having(thd, this, sel);
  if (item)
    item->marker|= MARKER_SUBSTITUTION;
  return item;
}


Item *Item_direct_view_ref::derived_field_transformer_for_having(THD *thd,
                                                                 uchar *arg)
{
  st_select_lex *sel= (st_select_lex *)arg;
  context= &sel->context;
  if ((*ref)->marker & MARKER_SUBSTITUTION)
  {
    this->marker|= MARKER_SUBSTITUTION;
    return this;
  }
  table_map tab_map= sel->master_unit()->derived->table->map;
  if ((item_equal && !(item_equal->used_tables() & tab_map)) ||
      !item_equal)
    return this;
  return get_field_item_for_having(thd, this, sel);
}


static 
Item *find_producing_item(Item *item, st_select_lex *sel)
{
  DBUG_ASSERT(item->type() == Item::FIELD_ITEM ||
              (item->type() == Item::REF_ITEM &&
               ((Item_ref *) item)->ref_type() == Item_ref::VIEW_REF)); 
  Item_field *field_item= NULL;
  Item_equal *item_equal= item->get_item_equal();
  table_map tab_map= sel->master_unit()->derived->table->map;
  if (item->used_tables() == tab_map)
    field_item= (Item_field *) (item->real_item());
  if (!field_item && item_equal)
  {
    Item_equal_fields_iterator it(*item_equal);
    Item *equal_item;
    while ((equal_item= it++))
    {
      if (equal_item->used_tables() == tab_map)
      {
        field_item= (Item_field *) (equal_item->real_item());
        break;
      }
    }
  }
  List_iterator_fast<Item> li(sel->item_list);
  if (field_item)
  {
    Item *producing_item= NULL;
    uint field_no= field_item->field->field_index;
    for (uint i= 0; i <= field_no; i++)
      producing_item= li++;
    return producing_item;
  }
  return NULL;
}

Item *Item_field::derived_field_transformer_for_where(THD *thd, uchar *arg)
{
  st_select_lex *sel= (st_select_lex *)arg;
  Item *producing_item= find_producing_item(this, sel);
  if (producing_item)
  {
    Item *producing_clone= producing_item->build_clone(thd);
    if (producing_clone)
      producing_clone->marker|= MARKER_SUBSTITUTION;
    return producing_clone;
  }
  return this;
}

Item *Item_direct_view_ref::derived_field_transformer_for_where(THD *thd,
                                                                uchar *arg)
{
  if ((*ref)->marker & MARKER_SUBSTITUTION)
    return (*ref);
  if (item_equal)
  {
    st_select_lex *sel= (st_select_lex *)arg;
    Item *producing_item= find_producing_item(this, sel);
    DBUG_ASSERT (producing_item != NULL);
    return producing_item->build_clone(thd);
  }
  return (*ref);
}


Item *Item_field::grouping_field_transformer_for_where(THD *thd, uchar *arg)
{
  st_select_lex *sel= (st_select_lex *)arg;
  Field_pair *gr_field= find_matching_field_pair(this, sel->grouping_tmp_fields);
  if (gr_field)
  {
    Item *producing_clone=
      gr_field->corresponding_item->build_clone(thd);
    if (producing_clone)
      producing_clone->marker|= MARKER_SUBSTITUTION;
    return producing_clone;
  }
  return this;
}


Item *
Item_direct_view_ref::grouping_field_transformer_for_where(THD *thd,
                                                           uchar *arg)
{
  if ((*ref)->marker & MARKER_SUBSTITUTION)
  {
    this->marker|= MARKER_SUBSTITUTION;
    return this;
  }
  if (!item_equal)
    return this;
  st_select_lex *sel= (st_select_lex *)arg;
  Field_pair *gr_field= find_matching_field_pair(this,
                                                 sel->grouping_tmp_fields);
  return gr_field->corresponding_item->build_clone(thd);
}

void Item_field::print(String *str, enum_query_type query_type)
{
  /*
    If the field refers to a constant table, print the value.
    (1): But don't attempt to do that if
          * the field refers to a temporary (work) table, and
          * temp. tables might already have been dropped.
  */
  if (!(refers_to_temp_table &&                      // (1)
        (query_type & QT_DONT_ACCESS_TMP_TABLES)) && // (1)
      field && field->table->const_table &&
      !(query_type & (QT_NO_DATA_EXPANSION | QT_VIEW_INTERNAL)))
  {
    print_value(str);
    return;
  }
  /*
    Item_ident doesn't have references to the underlying Field/TABLE objects,
    so it's safe to make the following call even when the table is not
    available already:
  */
  Item_ident::print(str, query_type);
}


Item_ref::Item_ref(THD *thd, Name_resolution_context *context_arg,
                   Item **item, const LEX_CSTRING &table_name_arg,
                   const LEX_CSTRING &field_name_arg,
                   bool alias_name_used_arg):
  Item_ident(thd, context_arg, null_clex_str, table_name_arg, field_name_arg),
  ref(item), reference_trough_name(0)
{
  alias_name_used= alias_name_used_arg;
  /*
    This constructor used to create some internals references over fixed items
  */
  if ((set_properties_only= (ref && *ref && (*ref)->fixed())))
    set_properties();
}

/*
  A Field_enumerator-compatible class that invokes mark_as_dependent() for
  each field that is a reference to some ancestor of current_select.
*/
class Dependency_marker: public Field_enumerator
{
public:
  THD *thd;
  st_select_lex *current_select;
  virtual void visit_field(Item_field *item)
  {
    // Find which select the field is in. This is achieved by walking up 
    // the select tree and looking for the table of interest.
    st_select_lex *sel;
    for (sel= current_select;
         sel ;
         sel= (sel->context.outer_context ?
               sel->context.outer_context->select_lex:
               NULL))
    {
      List_iterator<TABLE_LIST> li(sel->leaf_tables);
      TABLE_LIST *tbl;
      while ((tbl= li++))
      {
        if (tbl->table == item->field->table)
        {
          if (sel != current_select)
            mark_as_dependent(thd, sel, current_select, item, item, false);
          return;
        }
      }
    }
  }
};

Item_ref::Item_ref(THD *thd, TABLE_LIST *view_arg, Item **item,
                   const LEX_CSTRING &field_name_arg,
                   bool alias_name_used_arg):
  Item_ident(thd, view_arg, field_name_arg),
  ref(item), reference_trough_name(0)
{
  alias_name_used= alias_name_used_arg;
  /*
    This constructor is used to create some internal references over fixed items
  */
  if ((set_properties_only= (ref && *ref && (*ref)->fixed())))
    set_properties();
}


/**
  Resolve the name of a reference to a column reference.

  The method resolves the column reference represented by 'this' as a column
  present in one of: GROUP BY clause, SELECT clause, outer queries. It is
  used typically for columns in the HAVING clause which are not under
  aggregate functions.

  POSTCONDITION @n
  Item_ref::ref is 0 or points to a valid item.

  @note
    The name resolution algorithm used is (where [T_j] is an optional table
    name that qualifies the column name):

  @code
        resolve_extended([T_j].col_ref_i)
        {
          Search for a column or derived column named col_ref_i [in table T_j]
          in the SELECT and GROUP clauses of Q.

          if such a column is NOT found AND    // Lookup in outer queries.
             there are outer queries
          {
            for each outer query Q_k beginning from the inner-most one
           {
              Search for a column or derived column named col_ref_i
              [in table T_j] in the SELECT and GROUP clauses of Q_k.

              if such a column is not found AND
                 - Q_k is not a group query AND
                 - Q_k is not inside an aggregate function
                 OR
                 - Q_(k-1) is not in a HAVING or SELECT clause of Q_k
              {
                search for a column or derived column named col_ref_i
                [in table T_j] in the FROM clause of Q_k;
              }
            }
          }
        }
  @endcode
  @n
    This procedure treats GROUP BY and SELECT clauses as one namespace for
    column references in HAVING. Notice that compared to
    Item_field::fix_fields, here we first search the SELECT and GROUP BY
    clauses, and then we search the FROM clause.

  @param[in]     thd        current thread
  @param[in,out] reference  view column if this item was resolved to a
    view column

  @todo
    Here we could first find the field anyway, and then test this
    condition, so that we can give a better error message -
    ER_WRONG_FIELD_WITH_GROUP, instead of the less informative
    ER_BAD_FIELD_ERROR which we produce now.

  @retval
    TRUE  if error
  @retval
    FALSE on success
*/

bool Item_ref::fix_fields(THD *thd, Item **reference)
{
  enum_parsing_place place= NO_MATTER;
  DBUG_ASSERT(fixed() == 0);
  SELECT_LEX *current_sel= context->select_lex;

  if (set_properties_only)
  {
    /* do nothing */
  }
  else if (!ref || ref == not_found_item)
  {
    DBUG_ASSERT(reference_trough_name != 0);
    if (!(ref= resolve_ref_in_select_and_group(thd, this, context->select_lex)))
      goto error;             /* Some error occurred (e.g. ambiguous names). */

    if (ref == not_found_item) /* This reference was not resolved. */
    {
      Name_resolution_context *last_checked_context= context;
      Name_resolution_context *outer_context= context->outer_context;
      Field *from_field;
      ref= 0;

      if (unlikely(!outer_context))
      {
        /* The current reference cannot be resolved in this query. */
        my_error(ER_BAD_FIELD_ERROR,MYF(0), full_name(), thd->where);
        goto error;
      }

      /*
        If there is an outer context (select), and it is not a derived table
        (which do not support the use of outer fields for now), try to
        resolve this reference in the outer select(s).

        We treat each subselect as a separate namespace, so that different
        subselects may contain columns with the same names. The subselects are
        searched starting from the innermost.
      */
      from_field= (Field*) not_found_field;

      do
      {
        SELECT_LEX *select= outer_context->select_lex;
        Item_subselect *prev_subselect_item=
          last_checked_context->select_lex->master_unit()->item;
        last_checked_context= outer_context;

        /* Search in the SELECT and GROUP lists of the outer select. */
        if (outer_context->resolve_in_select_list)
        {
          if (!(ref= resolve_ref_in_select_and_group(thd, this, select)))
            goto error; /* Some error occurred (e.g. ambiguous names). */
          if (ref != not_found_item)
          {
            DBUG_ASSERT(*ref && (*ref)->fixed());
            prev_subselect_item->used_tables_and_const_cache_join(*ref);
            break;
          }
          /*
            Set ref to 0 to ensure that we get an error in case we replaced
            this item with another item and still use this item in some
            other place of the parse tree.
          */
          ref= 0;
        }

        place= prev_subselect_item->parsing_place;
        /*
          Check table fields only if the subquery is used somewhere out of
          HAVING or the outer SELECT does not use grouping (i.e. tables are
          accessible).
          TODO:
          Here we could first find the field anyway, and then test this
          condition, so that we can give a better error message -
          ER_WRONG_FIELD_WITH_GROUP, instead of the less informative
          ER_BAD_FIELD_ERROR which we produce now.
        */
        if ((place != IN_HAVING ||
             (!select->with_sum_func &&
              select->group_list.elements == 0)))
        {
          /*
            In case of view, find_field_in_tables() write pointer to view
            field expression to 'reference', i.e. it substitute that
            expression instead of this Item_ref
          */
          from_field= find_field_in_tables(thd, this,
                                           outer_context->
                                             first_name_resolution_table,
                                           outer_context->
                                             last_name_resolution_table,
                                           outer_context->ignored_tables,
                                           reference,
                                           IGNORE_EXCEPT_NON_UNIQUE,
                                           TRUE, TRUE);
          if (! from_field)
            goto error;
          if (from_field == view_ref_found)
          {
            Item::Type refer_type= (*reference)->type();
            prev_subselect_item->used_tables_and_const_cache_join(*reference);
            DBUG_ASSERT((*reference)->type() == REF_ITEM);
            mark_as_dependent(thd, last_checked_context->select_lex,
                              context->select_lex, this,
                              ((refer_type == REF_ITEM ||
                                refer_type == FIELD_ITEM) ?
                               (Item_ident*) (*reference) :
                               0), false);
            /*
              view reference found, we substituted it instead of this
              Item, so can quit
            */
            return FALSE;
          }
          if (from_field != not_found_field)
          {
            if (cached_table && cached_table->select_lex &&
                outer_context->select_lex &&
                cached_table->select_lex != outer_context->select_lex)
            {
              /*
                Due to cache, find_field_in_tables() can return field which
                doesn't belong to provided outer_context. In this case we have
                to find proper field context in order to fix field correctly.
              */
              do
              {
                outer_context= outer_context->outer_context;
                select= outer_context->select_lex;
                prev_subselect_item=
                  last_checked_context->select_lex->master_unit()->item;
                last_checked_context= outer_context;
              } while (outer_context && outer_context->select_lex &&
                       cached_table->select_lex != outer_context->select_lex);
            }
            prev_subselect_item->used_tables_cache|= from_field->table->map;
            prev_subselect_item->const_item_cache= 0;
            break;
          }
        }
        DBUG_ASSERT(from_field == not_found_field);

        /* Reference is not found => depend on outer (or just error). */
        prev_subselect_item->used_tables_cache|= OUTER_REF_TABLE_BIT;
        prev_subselect_item->const_item_cache= 0;

        outer_context= outer_context->outer_context;
      } while (outer_context);

      DBUG_ASSERT(from_field != 0 && from_field != view_ref_found);
      if (from_field != not_found_field)
      {
        Item_field* fld;
        if (!(fld= new (thd->mem_root) Item_field(thd, from_field)))
          goto error;
        thd->change_item_tree(reference, fld);
        mark_as_dependent(thd, last_checked_context->select_lex,
                          current_sel, fld, fld, false);
        /*
          A reference is resolved to a nest level that's outer or the same as
          the nest level of the enclosing set function : adjust the value of
          max_arg_level for the function if it's needed.
        */
        if (thd->lex->in_sum_func &&
            thd->lex == context->select_lex->parent_lex &&
            thd->lex->in_sum_func->nest_level >= 
            last_checked_context->select_lex->nest_level)
          set_if_bigger(thd->lex->in_sum_func->max_arg_level,
                        last_checked_context->select_lex->nest_level);
        return FALSE;
      }
      if (unlikely(ref == 0))
      {
        /* The item was not a table field and not a reference */
        my_error(ER_BAD_FIELD_ERROR, MYF(0),
                 this->full_name(), thd->where);
        goto error;
      }
      /* Should be checked in resolve_ref_in_select_and_group(). */
      DBUG_ASSERT(*ref && (*ref)->fixed());
      mark_as_dependent(thd, last_checked_context->select_lex,
                        context->select_lex, this, this, false);
      /*
        A reference is resolved to a nest level that's outer or the same as
        the nest level of the enclosing set function : adjust the value of
        max_arg_level for the function if it's needed.
      */
      if (thd->lex->in_sum_func &&
          thd->lex == context->select_lex->parent_lex &&
          thd->lex->in_sum_func->nest_level >= 
          last_checked_context->select_lex->nest_level)
        set_if_bigger(thd->lex->in_sum_func->max_arg_level,
                      last_checked_context->select_lex->nest_level);
    }
  }

  DBUG_ASSERT(*ref);
  /*
    Check if this is an incorrect reference in a group function or forward
    reference. Do not issue an error if this is:
      1. outer reference (will be fixed later by the fix_inner_refs function);
      2. an unnamed reference inside an aggregate function.
  */
  if (!((*ref)->type() == REF_ITEM &&
       ((Item_ref *)(*ref))->ref_type() == OUTER_REF) &&
      (((*ref)->with_sum_func() && name.str &&
        !(current_sel->get_linkage() != GLOBAL_OPTIONS_TYPE &&
          current_sel->having_fix_field)) ||
       !(*ref)->fixed()))
  {
    my_error(ER_ILLEGAL_REFERENCE, MYF(0),
             name.str, ((*ref)->with_sum_func() ?
                    "reference to group function":
                    "forward reference in item list"));
    goto error;
  }

  set_properties();

  if ((*ref)->check_cols(1))
    goto error;
  return FALSE;

error:
  context->process_error(thd);
  return TRUE;
}


void Item_ref::set_properties()
{
  Type_std_attributes::set(*ref);
  /*
    We have to remember if we refer to a sum function, to ensure that
    split_sum_func() doesn't try to change the reference.
  */
  with_flags= (*ref)->with_flags;
  base_flags|= (item_base_t::FIXED |
                ((*ref)->base_flags & item_base_t::MAYBE_NULL));

  if (alias_name_used)
    return;
  if ((*ref)->type() == FIELD_ITEM)
    alias_name_used= ((Item_ident *) (*ref))->alias_name_used;
  else
    alias_name_used= TRUE; // it is not field, so it is was resolved by alias
}


void Item_ref::cleanup()
{
  DBUG_ENTER("Item_ref::cleanup");
  Item_ident::cleanup();
  if (reference_trough_name)
  {
    /* We have to reset the reference as it may been freed */
    ref= 0;
  }
  DBUG_VOID_RETURN;
}


/**
  Transform an Item_ref object with a transformer callback function.

  The function first applies the transform method to the item
  referenced by this Item_ref object. If this returns a new item the
  old item is substituted for a new one. After this the transformer
  is applied to the Item_ref object.

  @param transformer   the transformer callback function to be applied to
                       the nodes of the tree of the object
  @param argument      parameter to be passed to the transformer

  @return Item returned as the result of transformation of the Item_ref object
    @retval !NULL The transformation was successful
    @retval NULL  Out of memory error
*/

Item* Item_ref::transform(THD *thd, Item_transformer transformer, uchar *arg)
{
  DBUG_ASSERT(!thd->stmt_arena->is_stmt_prepare());
  DBUG_ASSERT((*ref) != NULL);

  /* Transform the object we are referencing. */
  Item *new_item= (*ref)->transform(thd, transformer, arg);
  if (!new_item)
    return NULL;

  /*
    THD::change_item_tree() should be called only if the tree was
    really transformed, i.e. when a new item has been created.
    Otherwise we'll be allocating a lot of unnecessary memory for
    change records at each execution.
  */
  if (*ref != new_item)
    thd->change_item_tree(ref, new_item);

  /* Transform the item ref object. */
  return (this->*transformer)(thd, arg);
}


/**
  Compile an Item_ref object with a processor and a transformer
  callback functions.

  First the function applies the analyzer to the Item_ref object. Then
  if the analyzer succeeds we first apply the compile method to the
  object the Item_ref object is referencing. If this returns a new
  item the old item is substituted for a new one. After this the
  transformer is applied to the Item_ref object itself.
  The compile function is not called if the analyzer returns NULL
  in the parameter arg_p. 

  @param analyzer      the analyzer callback function to be applied to the
                       nodes of the tree of the object
  @param[in,out] arg_p parameter to be passed to the processor
  @param transformer   the transformer callback function to be applied to the
                       nodes of the tree of the object
  @param arg_t         parameter to be passed to the transformer

  @return Item returned as the result of transformation of the Item_ref object
*/

Item* Item_ref::compile(THD *thd, Item_analyzer analyzer, uchar **arg_p,
                        Item_transformer transformer, uchar *arg_t)
{
  /* Analyze this Item object. */
  if (!(this->*analyzer)(arg_p))
    return NULL;

  /* Compile the Item we are referencing. */
  DBUG_ASSERT((*ref) != NULL);
  if (*arg_p)
  {
    uchar *arg_v= *arg_p;
    Item *new_item= (*ref)->compile(thd, analyzer, &arg_v, transformer, arg_t);
    if (new_item && *ref != new_item)
      thd->change_item_tree(ref, new_item);
  }

  /* Transform this Item object. */
  return (this->*transformer)(thd, arg_t);
}


void Item_ref::print(String *str, enum_query_type query_type)
{
  if (ref)
  {
    if ((*ref)->type() != Item::CACHE_ITEM &&
        (*ref)->type() != Item::WINDOW_FUNC_ITEM &&
        ref_type() != VIEW_REF &&
        !table_name.str && name.str && alias_name_used)
    {
      THD *thd= current_thd;
      append_identifier(thd, str, &(*ref)->real_item()->name);
    }
    else
      (*ref)->print(str, query_type);
  }
  else
    Item_ident::print(str, query_type);
}


bool Item_ref::send(Protocol *prot, st_value *buffer)
{
  if (result_field)
    return prot->store(result_field);
  return (*ref)->send(prot, buffer);
}


double Item_ref::val_result()
{
  if (result_field)
  {
    if ((null_value= result_field->is_null()))
      return 0.0;
    return result_field->val_real();
  }
  return val_real();
}


bool Item_ref::is_null_result()
{
  if (result_field)
    return (null_value=result_field->is_null());

  return is_null();
}


longlong Item_ref::val_int_result()
{
  if (result_field)
  {
    if ((null_value= result_field->is_null()))
      return 0;
    return result_field->val_int();
  }
  return val_int();
}


String *Item_ref::str_result(String* str)
{
  if (result_field)
  {
    if ((null_value= result_field->is_null()))
      return 0;
    str->set_charset(str_value.charset());
    return result_field->val_str(str, &str_value);
  }
  return val_str(str);
}


bool Item_ref::val_native_result(THD *thd, Native *to)
{
  return result_field ?
         val_native_from_field(result_field, to) :
         val_native(thd, to);
}


my_decimal *Item_ref::val_decimal_result(my_decimal *decimal_value)
{
  if (result_field)
  {
    if ((null_value= result_field->is_null()))
      return 0;
    return result_field->val_decimal(decimal_value);
  }
  return val_decimal(decimal_value);
}


bool Item_ref::val_bool_result()
{
  if (result_field)
  {
    if ((null_value= result_field->is_null()))
      return false;
    return result_field->val_bool();
  }
  return val_bool();
}


void Item_ref::save_result(Field *to)
{
  if (result_field)
  {
    save_field_in_field(result_field, &null_value, to, TRUE);
    return;
  }
  (*ref)->save_result(to);
  null_value= (*ref)->null_value;
}


void Item_ref::save_val(Field *to)
{
  (*ref)->save_result(to);
  null_value= (*ref)->null_value;
}


double Item_ref::val_real()
{
  DBUG_ASSERT(fixed());
  double tmp=(*ref)->val_result();
  null_value=(*ref)->null_value;
  return tmp;
}


longlong Item_ref::val_int()
{
  DBUG_ASSERT(fixed());
  longlong tmp=(*ref)->val_int_result();
  null_value=(*ref)->null_value;
  return tmp;
}


bool Item_ref::val_bool()
{
  DBUG_ASSERT(fixed());
  bool tmp= (*ref)->val_bool_result();
  null_value= (*ref)->null_value;
  return tmp;
}


String *Item_ref::val_str(String* tmp)
{
  DBUG_ASSERT(fixed());
  tmp=(*ref)->str_result(tmp);
  null_value=(*ref)->null_value;
  return tmp;
}


bool Item_ref::is_null()
{
  DBUG_ASSERT(fixed());
  bool tmp=(*ref)->is_null_result();
  null_value=(*ref)->null_value;
  return tmp;
}


bool Item_ref::get_date(THD *thd, MYSQL_TIME *ltime, date_mode_t fuzzydate)
{
  return (null_value=(*ref)->get_date_result(thd, ltime, fuzzydate));
}


bool Item_ref::val_native(THD *thd, Native *to)
{
  return val_native_from_item(thd, *ref, to);
}


longlong Item_ref::val_datetime_packed(THD *thd)
{
  DBUG_ASSERT(fixed());
  longlong tmp= (*ref)->val_datetime_packed_result(thd);
  null_value= (*ref)->null_value;
  return tmp;
}


longlong Item_ref::val_time_packed(THD *thd)
{
  DBUG_ASSERT(fixed());
  longlong tmp= (*ref)->val_time_packed_result(thd);
  null_value= (*ref)->null_value;
  return tmp;
}


my_decimal *Item_ref::val_decimal(my_decimal *decimal_value)
{
  my_decimal *val= (*ref)->val_decimal_result(decimal_value);
  null_value= (*ref)->null_value;
  return val;
}

int Item_ref::save_in_field(Field *to, bool no_conversions)
{
  int res;
  if (result_field)
  {
    if (result_field->is_null())
    {
      null_value= 1;
      res= set_field_to_null_with_conversions(to, no_conversions);
      return res;
    }
    to->set_notnull();
    res= field_conv(to, result_field);
    null_value= 0;
    return res;
  }
  res= (*ref)->save_in_field(to, no_conversions);
  null_value= (*ref)->null_value;
  return res;
}


void Item_ref::save_org_in_field(Field *field, fast_field_copier optimizer_data)
{
  (*ref)->save_org_in_field(field, optimizer_data);
}


void Item_ref::make_send_field(THD *thd, Send_field *field)
{
  (*ref)->make_send_field(thd, field);
  /* Non-zero in case of a view */
  if (name.str)
    field->col_name= name;
  if (table_name.str)
    field->table_name= table_name;
  if (db_name.str)
    field->db_name= db_name;
  if (orig_field_name.str)
    field->org_col_name= orig_field_name;
  if (orig_table_name.str)
    field->org_table_name= orig_table_name;
}


Item *Item_ref::get_tmp_table_item(THD *thd)
{
  if (!result_field)
    return (*ref)->get_tmp_table_item(thd);

  Item_field *item= new (thd->mem_root) Item_field(thd, result_field);
  if (item)
  {
    item->table_name= table_name;
    item->db_name= db_name;
  }
  return item;
}


void Item_ref_null_helper::print(String *str, enum_query_type query_type)
{
  str->append(STRING_WITH_LEN("<ref_null_helper>("));
  if (ref)
    (*ref)->print(str, query_type);
  else
    str->append('?');
  str->append(')');
}


void Item_direct_ref::save_val(Field *to)
{
  (*ref)->save_val(to);
  null_value=(*ref)->null_value;
}


double Item_direct_ref::val_real()
{
  double tmp=(*ref)->val_real();
  null_value=(*ref)->null_value;
  return tmp;
}


longlong Item_direct_ref::val_int()
{
  longlong tmp=(*ref)->val_int();
  null_value=(*ref)->null_value;
  return tmp;
}


String *Item_direct_ref::val_str(String* tmp)
{
  tmp=(*ref)->val_str(tmp);
  null_value=(*ref)->null_value;
  return tmp;
}


my_decimal *Item_direct_ref::val_decimal(my_decimal *decimal_value)
{
  my_decimal *tmp= (*ref)->val_decimal(decimal_value);
  null_value=(*ref)->null_value;
  return tmp;
}


bool Item_direct_ref::val_bool()
{
  bool tmp= (*ref)->val_bool();
  null_value=(*ref)->null_value;
  return tmp;
}


bool Item_direct_ref::is_null()
{
  return (*ref)->is_null();
}


bool Item_direct_ref::get_date(THD *thd, MYSQL_TIME *ltime, date_mode_t fuzzydate)
{
  return (null_value=(*ref)->get_date(thd, ltime, fuzzydate));
}


bool Item_direct_ref::val_native(THD *thd, Native *to)
{
  return val_native_from_item(thd, *ref, to);
}


longlong Item_direct_ref::val_time_packed(THD *thd)
{
  longlong tmp = (*ref)->val_time_packed(thd);
  null_value= (*ref)->null_value;
  return tmp;
}


longlong Item_direct_ref::val_datetime_packed(THD *thd)
{
  longlong tmp = (*ref)->val_datetime_packed(thd);
  null_value= (*ref)->null_value;
  return tmp;
}


Item_cache_wrapper::~Item_cache_wrapper()
{
  DBUG_ASSERT(expr_cache == 0);
}

Item_cache_wrapper::Item_cache_wrapper(THD *thd, Item *item_arg):
  Item_result_field(thd), orig_item(item_arg), expr_cache(NULL), expr_value(NULL)
{
  DBUG_ASSERT(orig_item->fixed());
  Type_std_attributes::set(orig_item);

  base_flags|= (item_base_t::FIXED |
                (orig_item->base_flags & item_base_t::MAYBE_NULL));
  with_flags|= orig_item->with_flags;

  name= item_arg->name;

  if ((expr_value= orig_item->get_cache(thd)))
    expr_value->setup(thd, orig_item);
}


/**
  Initialize the cache if it is needed
*/

void Item_cache_wrapper::init_on_demand()
{
    if (!expr_cache->is_inited())
    {
      orig_item->get_cache_parameters(parameters);
      expr_cache->init();
    }
}


void Item_cache_wrapper::print(String *str, enum_query_type query_type)
{
  if (query_type & QT_ITEM_CACHE_WRAPPER_SKIP_DETAILS)
  {
    /* Don't print the cache in EXPLAIN EXTENDED */
    orig_item->print(str, query_type);
    return;
  }

  str->append(STRING_WITH_LEN("<expr_cache>"));
  if (expr_cache)
  {
    init_on_demand();
    expr_cache->print(str, query_type);
  }
  else
    str->append(STRING_WITH_LEN("<<DISABLED>>"));
  str->append('(');
  orig_item->print(str, query_type);
  str->append(')');
}


/**
  Prepare the expression cache wrapper (do nothing)

  @retval FALSE OK
*/

bool Item_cache_wrapper::fix_fields(THD *thd  __attribute__((unused)),
                                    Item **it __attribute__((unused)))
{
  DBUG_ASSERT(orig_item->fixed());
  DBUG_ASSERT(fixed());
  return FALSE;
}

bool Item_cache_wrapper::send(Protocol *protocol, st_value *buffer)
{
  if (result_field)
    return protocol->store(result_field);
  return Item::send(protocol, buffer);
}

/**
  Clean the expression cache wrapper up before reusing it.
*/

void Item_cache_wrapper::cleanup()
{
  DBUG_ENTER("Item_cache_wrapper::cleanup");
  Item_result_field::cleanup();
  delete expr_cache;
  expr_cache= 0;
  /* expr_value is Item so it will be destroyed from list of Items */
  expr_value= 0;
  parameters.empty();
  DBUG_VOID_RETURN;
}


/**
  Create an expression cache that uses a temporary table

  @param thd           Thread handle
  @param depends_on    Parameters of the expression to create cache for

  @details
  The function takes 'depends_on' as the list of all parameters for
  the expression wrapped into this object and creates an expression
  cache in a temporary table containing the field for the parameters
  and the result of the expression.

  @retval FALSE OK
  @retval TRUE  Error
*/

bool Item_cache_wrapper::set_cache(THD *thd)
{
  DBUG_ENTER("Item_cache_wrapper::set_cache");
  DBUG_ASSERT(expr_cache == 0);
  expr_cache= new Expression_cache_tmptable(thd, parameters, expr_value);
  DBUG_RETURN(expr_cache == NULL);
}

Expression_cache_tracker* Item_cache_wrapper::init_tracker(MEM_ROOT *mem_root)
{
  if (expr_cache)
  {
    Expression_cache_tracker* tracker=
      new(mem_root) Expression_cache_tracker(expr_cache);
    if (tracker)
      ((Expression_cache_tmptable *)expr_cache)->set_tracker(tracker);
    return tracker;
  }
  return NULL;
}


/**
  Check if the current values of the parameters are in the expression cache

  @details
  The function checks whether the current set of the parameters of the
  referenced item can be found in the expression cache. If so the function
  returns the item by which the result of the expression can be easily
  extracted from the cache with the corresponding val_* method.

  @retval NULL    - parameters are not in the cache
  @retval <item*> - item providing the result of the expression found in cache
*/

Item *Item_cache_wrapper::check_cache()
{
  DBUG_ENTER("Item_cache_wrapper::check_cache");
  if (expr_cache)
  {
    Expression_cache_tmptable::result res;
    Item *cached_value;
    init_on_demand();
    res= expr_cache->check_value(&cached_value);
    if (res == Expression_cache_tmptable::HIT)
      DBUG_RETURN(cached_value);
  }
  DBUG_RETURN(NULL);
}


/**
  Get the value of the cached expression and put it in the cache
*/

inline void Item_cache_wrapper::cache()
{
  expr_value->store(orig_item);
  expr_value->cache_value();
  expr_cache->put_value(expr_value); // put in expr_cache
}


/**
  Get the value of the possibly cached item into the field.
*/

void Item_cache_wrapper::save_val(Field *to)
{
  Item *cached_value;
  DBUG_ENTER("Item_cache_wrapper::val_int");
  if (!expr_cache)
  {
    orig_item->save_val(to);
    null_value= orig_item->null_value;
    DBUG_VOID_RETURN;
  }

  if ((cached_value= check_cache()))
  {
    cached_value->save_val(to);
    null_value= cached_value->null_value;
    DBUG_VOID_RETURN;
  }
  cache();
  null_value= expr_value->null_value;
  expr_value->save_val(to);
  DBUG_VOID_RETURN;
}


/**
  Get the integer value of the possibly cached item.
*/

longlong Item_cache_wrapper::val_int()
{
  Item *cached_value;
  DBUG_ENTER("Item_cache_wrapper::val_int");
  if (!expr_cache)
  {
    longlong tmp= orig_item->val_int();
    null_value= orig_item->null_value;
    DBUG_RETURN(tmp);
  }

  if ((cached_value= check_cache()))
  {
    longlong tmp= cached_value->val_int();
    null_value= cached_value->null_value;
    DBUG_RETURN(tmp);
  }
  cache();
  null_value= expr_value->null_value;
  DBUG_RETURN(expr_value->val_int());
}


/**
  Get the real value of the possibly cached item
*/

double Item_cache_wrapper::val_real()
{
  Item *cached_value;
  DBUG_ENTER("Item_cache_wrapper::val_real");
  if (!expr_cache)
  {
    double tmp= orig_item->val_real();
    null_value= orig_item->null_value;
    DBUG_RETURN(tmp);
  }

  if ((cached_value= check_cache()))
  {
    double tmp= cached_value->val_real();
    null_value= cached_value->null_value;
    DBUG_RETURN(tmp);
  }
  cache();
  null_value= expr_value->null_value;
  DBUG_RETURN(expr_value->val_real());
}


/**
  Get the string value of the possibly cached item
*/

String *Item_cache_wrapper::val_str(String* str)
{
  Item *cached_value;
  DBUG_ENTER("Item_cache_wrapper::val_str");
  if (!expr_cache)
  {
    String *tmp= orig_item->val_str(str);
    null_value= orig_item->null_value;
    DBUG_RETURN(tmp);
  }

  if ((cached_value= check_cache()))
  {
    String *tmp= cached_value->val_str(str);
    null_value= cached_value->null_value;
    DBUG_RETURN(tmp);
  }
  cache();
  if ((null_value= expr_value->null_value))
    DBUG_RETURN(NULL);
  DBUG_RETURN(expr_value->val_str(str));
}


/**
  Get the native value of the possibly cached item
*/

bool Item_cache_wrapper::val_native(THD *thd, Native* to)
{
  Item *cached_value;
  DBUG_ENTER("Item_cache_wrapper::val_native");
  if (!expr_cache)
    DBUG_RETURN(val_native_from_item(thd, orig_item, to));

  if ((cached_value= check_cache()))
    DBUG_RETURN(val_native_from_item(thd, cached_value, to));

  cache();
  if ((null_value= expr_value->null_value))
    DBUG_RETURN(true);
  DBUG_RETURN(expr_value->val_native(thd, to));
}



/**
  Get the decimal value of the possibly cached item
*/

my_decimal *Item_cache_wrapper::val_decimal(my_decimal* decimal_value)
{
  Item *cached_value;
  DBUG_ENTER("Item_cache_wrapper::val_decimal");
  if (!expr_cache)
  {
    my_decimal *tmp= orig_item->val_decimal(decimal_value);
    null_value= orig_item->null_value;
    DBUG_RETURN(tmp);
  }

  if ((cached_value= check_cache()))
  {
    my_decimal *tmp= cached_value->val_decimal(decimal_value);
    null_value= cached_value->null_value;
    DBUG_RETURN(tmp);
  }
  cache();
  if ((null_value= expr_value->null_value))
    DBUG_RETURN(NULL);
  DBUG_RETURN(expr_value->val_decimal(decimal_value));
}


/**
  Get the boolean value of the possibly cached item
*/

bool Item_cache_wrapper::val_bool()
{
  Item *cached_value;
  DBUG_ENTER("Item_cache_wrapper::val_bool");
  if (!expr_cache)
  {
    bool tmp= orig_item->val_bool();
    null_value= orig_item->null_value;
    DBUG_RETURN(tmp);
  }

  if ((cached_value= check_cache()))
  {
    bool tmp= cached_value->val_bool();
    null_value= cached_value->null_value;
    DBUG_RETURN(tmp);
  }
  cache();
  null_value= expr_value->null_value;
  DBUG_RETURN(expr_value->val_bool());
}


/**
  Check for NULL the value of the possibly cached item
*/

bool Item_cache_wrapper::is_null()
{
  Item *cached_value;
  DBUG_ENTER("Item_cache_wrapper::is_null");
  if (!expr_cache)
  {
    bool tmp= orig_item->is_null();
    null_value= orig_item->null_value;
    DBUG_RETURN(tmp);
  }

  if ((cached_value= check_cache()))
  {
    bool tmp= cached_value->is_null();
    null_value= cached_value->null_value;
    DBUG_RETURN(tmp);
  }
  cache();
  DBUG_RETURN((null_value= expr_value->null_value));
}


/**
  Get the date value of the possibly cached item
*/

bool Item_cache_wrapper::get_date(THD *thd, MYSQL_TIME *ltime,
                                  date_mode_t fuzzydate)
{
  Item *cached_value;
  DBUG_ENTER("Item_cache_wrapper::get_date");
  if (!expr_cache)
    DBUG_RETURN((null_value= orig_item->get_date(thd, ltime, fuzzydate)));

  if ((cached_value= check_cache()))
    DBUG_RETURN((null_value= cached_value->get_date(thd, ltime, fuzzydate)));

  cache();
  DBUG_RETURN((null_value= expr_value->get_date(thd, ltime, fuzzydate)));
}


int Item_cache_wrapper::save_in_field(Field *to, bool no_conversions)
{
  int res;
  DBUG_ASSERT(!result_field);
  res= orig_item->save_in_field(to, no_conversions);
  null_value= orig_item->null_value;
  return res;
}


Item* Item_cache_wrapper::get_tmp_table_item(THD *thd)
{
  if (!orig_item->with_sum_func() && !orig_item->const_item())
  {
    auto item_field= new (thd->mem_root) Item_field(thd, result_field);
    if (item_field)
      item_field->set_refers_to_temp_table(true);
    return item_field;
  }
  return copy_or_same(thd);
}


bool Item_direct_view_ref::send(Protocol *protocol, st_value *buffer)
{
  if (check_null_ref())
    return protocol->store_null();
  return Item_direct_ref::send(protocol, buffer);
}

/**
  Prepare referenced field then call usual Item_direct_ref::fix_fields .

  @param thd         thread handler
  @param reference   reference on reference where this item stored

  @retval
    FALSE   OK
  @retval
    TRUE    Error
*/

bool Item_direct_view_ref::fix_fields(THD *thd, Item **reference)
{
  /* view fild reference must be defined */
  DBUG_ASSERT(*ref);
  /* (*ref)->check_cols() will be made in Item_direct_ref::fix_fields */
  if ((*ref)->fixed())
  {
    Item *ref_item= (*ref)->real_item();
    if (ref_item->type() == Item::FIELD_ITEM)
    {
      /*
        In some cases we need to update table read set(see bug#47150).
        If ref item is FIELD_ITEM and fixed then field and table
        have proper values. So we can use them for update.
      */
      Field *fld= ((Item_field*) ref_item)->field;
      DBUG_ASSERT(fld && fld->table);
      if (thd->column_usage == MARK_COLUMNS_READ)
        bitmap_set_bit(fld->table->read_set, fld->field_index);
    }
  }
  else if ((*ref)->fix_fields_if_needed(thd, ref))
    return TRUE;

  if (Item_direct_ref::fix_fields(thd, reference))
    return TRUE;
  if (view->table && view->table->maybe_null)
    set_maybe_null();
  set_null_ref_table();
  return FALSE;
}

/*
  Prepare referenced outer field then call usual Item_direct_ref::fix_fields

  SYNOPSIS
    Item_outer_ref::fix_fields()
    thd         thread handler
    reference   reference on reference where this item stored

  RETURN
    FALSE   OK
    TRUE    Error
*/

bool Item_outer_ref::fix_fields(THD *thd, Item **reference)
{
  bool err;
  /* outer_ref->check_cols() will be made in Item_direct_ref::fix_fields */
  if ((*ref) && (*ref)->fix_fields_if_needed(thd, reference))
    return TRUE;
  err= Item_direct_ref::fix_fields(thd, reference);
  if (!outer_ref)
    outer_ref= *ref;
  if ((*ref)->type() == Item::FIELD_ITEM)
    table_name= ((Item_field*)outer_ref)->table_name;
  return err;
}


void Item_outer_ref::fix_after_pullout(st_select_lex *new_parent,
                                       Item **ref_arg, bool merge)
{
  if (get_depended_from() == new_parent)
  {
    *ref_arg= outer_ref;
    (*ref_arg)->fix_after_pullout(new_parent, ref_arg, merge);
  }
}

void Item_ref::fix_after_pullout(st_select_lex *new_parent, Item **refptr,
                                 bool merge)
{
  (*ref)->fix_after_pullout(new_parent, ref, merge);
  if (get_depended_from() == new_parent)
    depended_from= NULL;
}


/**
  Mark references from inner selects used in group by clause

  The method is used by the walk method when called for the expressions
  from the group by clause. The callsare  occurred in the function
  fix_inner_refs invoked by JOIN::prepare.
  The parameter passed to Item_outer_ref::check_inner_refs_processor
  is the iterator over the list of inner references from the subselects
  of the select to be prepared. The function marks those references
  from this list whose occurrences are encountered in the group by 
  expressions passed to the walk method.  
 
  @param arg  pointer to the iterator over a list of inner references

  @return
    FALSE always
*/

bool Item_outer_ref::check_inner_refs_processor(void *arg)
{
  List_iterator_fast<Item_outer_ref> *it=
    ((List_iterator_fast<Item_outer_ref> *) arg);
  Item_outer_ref *tmp_ref;
  while ((tmp_ref= (*it)++))
  {
    if (tmp_ref == this)
    {
      tmp_ref->found_in_group_by= 1;
      break;
    }
  }
  (*it).rewind();
  return FALSE;
}


/**
  Compare two view column references for equality.

  A view column reference is considered equal to another column
  reference if the second one is a view column and if both column
  references resolve to the same item. It is assumed that both
  items are of the same type.

  @param item        item to compare with
  @param binary_cmp  make binary comparison

  @retval
    TRUE    Referenced item is equal to given item
  @retval
    FALSE   otherwise
*/

bool Item_direct_view_ref::eq(const Item *item, bool binary_cmp) const
{
  if (item->type() == REF_ITEM)
  {
    Item_ref *item_ref= (Item_ref*) item;
    if (item_ref->ref_type() == VIEW_REF)
    {
      Item *item_ref_ref= *(item_ref->ref);
      return ((*ref)->real_item() == item_ref_ref->real_item());
    }
  }
  return FALSE;
}


Item_equal *Item_direct_view_ref::find_item_equal(COND_EQUAL *cond_equal)
{
  Item* field_item= real_item();
  if (field_item->type() != FIELD_ITEM)
    return NULL;
  return ((Item_field *) field_item)->find_item_equal(cond_equal);  
}


/**
  Set a pointer to the multiple equality the view field reference belongs to
  (if any).

  @details
  The function looks for a multiple equality containing this item of the type
  Item_direct_view_ref among those referenced by arg.
  In the case such equality exists the function does the following.
  If the found multiple equality contains a constant, then the item
  is substituted for this constant, otherwise the function sets a pointer
  to the multiple equality in the item.

  @param arg    reference to list of multiple equalities where
                the item (this object) is to be looked for

  @note
    This function is supposed to be called as a callback parameter in calls
    of the compile method.

  @note 
    The function calls Item_field::propagate_equal_fields() for the field item
    this->real_item() to do the job. Then it takes the pointer to equal_item
    from this field item and assigns it to this->item_equal.

  @return
    - pointer to the replacing constant item, if the field item was substituted
    - pointer to the field item, otherwise.
*/

Item *Item_direct_view_ref::propagate_equal_fields(THD *thd,
                                                   const Context &ctx,
                                                   COND_EQUAL *cond)
{
  Item *field_item= real_item();
  if (field_item->type() != FIELD_ITEM)
    return this;
  Item *item= field_item->propagate_equal_fields(thd, ctx, cond);
  set_item_equal(field_item->get_item_equal());
  field_item->set_item_equal(NULL);
  if (item != field_item)
    return item;
  return this;
}


Item *Item_ref::propagate_equal_fields(THD *thd, const Context &ctx,
                                       COND_EQUAL *cond)
{
  Item *field_item= real_item();
  if (field_item->type() != FIELD_ITEM)
    return this;
  Item *item= field_item->propagate_equal_fields(thd, ctx, cond);
  if (item != field_item)
    return item;
  return this;
}


/**
  Replace an Item_direct_view_ref for an equal Item_field evaluated earlier
  (if any).

  @details
  If this->item_equal points to some item and coincides with arg then
  the function returns a pointer to a field item that is referred to by the 
  first element of the item_equal list which the Item_direct_view_ref
  object belongs to unless item_equal contains  a constant item. In this
  case the function returns this constant item (if the substitution does
   not require conversion).   
  If the Item_direct_view_ref object does not refer any Item_equal object
  'this' is returned .

  @param arg   NULL or points to so some item of the Item_equal type  

  @note
    This function is supposed to be called as a callback parameter in calls
    of the transformer method.

  @note 
    The function calls Item_field::replace_equal_field for the field item
    this->real_item() to do the job.

  @return
    - pointer to a replacement Item_field if there is a better equal item or
      a pointer to a constant equal item;
    - this - otherwise.
*/

Item *Item_direct_view_ref::replace_equal_field(THD *thd, uchar *arg)
{
  Item *field_item= real_item();
  if (field_item->type() != FIELD_ITEM)
    return this;
  field_item->set_item_equal(item_equal);
  Item *item= field_item->replace_equal_field(thd, arg);
  field_item->set_item_equal(0);
  return item != field_item ? item : this;
}


bool Item_field::excl_dep_on_table(table_map tab_map)
{
  return used_tables() == tab_map ||
         (item_equal && (item_equal->used_tables() & tab_map));
}


bool
Item_field::excl_dep_on_grouping_fields(st_select_lex *sel)
{
  return find_matching_field_pair(this, sel->grouping_tmp_fields) != NULL;
}


bool Item_direct_view_ref::excl_dep_on_table(table_map tab_map)
{
  table_map used= used_tables();
  if (used & (OUTER_REF_TABLE_BIT | RAND_TABLE_BIT))
    return false;
  if (!(used & ~tab_map))
    return true; 
  if (item_equal)
  {
    DBUG_ASSERT(real_item()->type() == Item::FIELD_ITEM);
    return item_equal->used_tables() & tab_map;
  }
  return (*ref)->excl_dep_on_table(tab_map);
}


bool Item_direct_view_ref::excl_dep_on_grouping_fields(st_select_lex *sel)
{
  if (item_equal)
  {
    DBUG_ASSERT(real_item()->type() == Item::FIELD_ITEM);
    return (find_matching_field_pair(this, sel->grouping_tmp_fields) != NULL);
  }    
  return (*ref)->excl_dep_on_grouping_fields(sel);
}


bool Item_args::excl_dep_on_grouping_fields(st_select_lex *sel)
{
  for (uint i= 0; i < arg_count; i++)
  {
    if (args[i]->type() == Item::FUNC_ITEM &&
        ((Item_func *)args[i])->functype() == Item_func::UDF_FUNC)
      return false;
    if (args[i]->const_item())
      continue;
    if (!args[i]->excl_dep_on_grouping_fields(sel))
      return false;
  }
  return true;
}


double Item_direct_view_ref::val_result()
{
  double tmp=(*ref)->val_result();
  null_value=(*ref)->null_value;
  return tmp;
}


longlong Item_direct_view_ref::val_int_result()
{
  longlong tmp=(*ref)->val_int_result();
  null_value=(*ref)->null_value;
  return tmp;
}


String *Item_direct_view_ref::str_result(String* tmp)
{
  tmp=(*ref)->str_result(tmp);
  null_value=(*ref)->null_value;
  return tmp;
}


my_decimal *Item_direct_view_ref::val_decimal_result(my_decimal *val)
{
  my_decimal *tmp= (*ref)->val_decimal_result(val);
  null_value=(*ref)->null_value;
  return tmp;
}


bool Item_direct_view_ref::val_bool_result()
{
  bool tmp= (*ref)->val_bool_result();
  null_value=(*ref)->null_value;
  return tmp;
}


bool Item_default_value::eq(const Item *item, bool binary_cmp) const
{
  return item->type() == DEFAULT_VALUE_ITEM && 
    ((Item_default_value *)item)->arg->eq(arg, binary_cmp);
}


bool Item_default_value::check_field_expression_processor(void *)
{
  field->default_value= ((Item_field *)(arg->real_item()))->field->default_value;
  return 0;
}

bool Item_default_value::fix_fields(THD *thd, Item **items)
{
  Item *real_arg;
  Item_field *field_arg;
  Field *def_field;
  DBUG_ASSERT(fixed() == 0);
  DBUG_ASSERT(arg);

  /*
    DEFAULT() do not need table field so should not ask handler to bring
    field value (mark column for read)
  */
  enum_column_usage save_column_usage= thd->column_usage;
  /*
    Fields which has defult value could be read, so it is better hide system
    invisible columns.
  */
  thd->column_usage= COLUMNS_WRITE;
  if (arg->fix_fields_if_needed(thd, &arg))
  {
    thd->column_usage= save_column_usage;
    goto error;
  }
  thd->column_usage= save_column_usage;

  real_arg= arg->real_item();
  if (real_arg->type() != FIELD_ITEM)
  {
    my_error(ER_NO_DEFAULT_FOR_FIELD, MYF(0), arg->name.str);
    goto error;
  }

  field_arg= (Item_field *)real_arg;
  if ((field_arg->field->flags & NO_DEFAULT_VALUE_FLAG))
  {
    my_error(ER_NO_DEFAULT_FOR_FIELD, MYF(0),
             field_arg->field->field_name.str);
    goto error;
  }
  if (!(def_field= (Field*) thd->alloc(field_arg->field->size_of())))
    goto error;
  memcpy((void *)def_field, (void *)field_arg->field,
         field_arg->field->size_of());
  def_field->reset_fields();
  // If non-constant default value expression or a blob
  if (def_field->default_value &&
      (def_field->default_value->flags || (def_field->flags & BLOB_FLAG)))
  {
    uchar *newptr= (uchar*) thd->alloc(1+def_field->pack_length());
    if (!newptr)
      goto error;
    if (should_mark_column(thd->column_usage))
      def_field->default_value->expr->update_used_tables();
    def_field->move_field(newptr+1, def_field->maybe_null() ? newptr : 0, 1);
  }
  else
    def_field->move_field_offset((my_ptrdiff_t)
                                 (def_field->table->s->default_values -
                                  def_field->table->record[0]));
  set_field(def_field);
  return FALSE;

error:
  context->process_error(thd);
  return TRUE;
}

void Item_default_value::cleanup()
{
  delete field;                        // Free cached blob data
  Item_field::cleanup();
}

void Item_default_value::print(String *str, enum_query_type query_type)
{
  DBUG_ASSERT(arg);
  str->append(STRING_WITH_LEN("default("));
  /*
    We take DEFAULT from a field so do not need it value in case of const
    tables but its name so we set QT_NO_DATA_EXPANSION (as we print for
    table definition, also we do not need table and database name)
  */
  query_type= (enum_query_type) (query_type | QT_NO_DATA_EXPANSION);
  arg->print(str, query_type);
  str->append(')');
}

void Item_default_value::calculate()
{
  DBUG_ASSERT(arg);
  if (field->default_value)
    field->set_default();
  DEBUG_SYNC(field->table->in_use, "after_Item_default_value_calculate");
}

bool Item_default_value::val_native(THD *thd, Native *to)
{
  calculate();
  return Item_field::val_native(thd, to);
}

String *Item_default_value::val_str(String *str)
{
  calculate();
  return Item_field::val_str(str);
}

double Item_default_value::val_real()
{
  calculate();
  return Item_field::val_real();
}

longlong Item_default_value::val_int()
{
  calculate();
  return Item_field::val_int();
}

my_decimal *Item_default_value::val_decimal(my_decimal *decimal_value)
{
  calculate();
  return Item_field::val_decimal(decimal_value);
}

bool Item_default_value::get_date(THD *thd, MYSQL_TIME *ltime,
                                  date_mode_t fuzzydate)
{
  calculate();
  return Item_field::get_date(thd, ltime, fuzzydate);
}

bool Item_default_value::send(Protocol *protocol, st_value *buffer)
{
  calculate();
  return Item_field::send(protocol, buffer);
}

int Item_default_value::save_in_field(Field *field_arg, bool no_conversions)
{
  calculate();
  return Item_field::save_in_field(field_arg, no_conversions);
}

void Item_default_value::save_in_result_field(bool no_conversions)
{
  calculate();
  Item_field::save_in_result_field(no_conversions);
}

double Item_default_value::val_result()
{
  calculate();
  return Item_field::val_result();
}

longlong Item_default_value::val_int_result()
{
  calculate();
  return Item_field::val_int_result();
}

String *Item_default_value::str_result(String* tmp)
{
  calculate();
  return Item_field::str_result(tmp);
}

bool Item_default_value::val_bool_result()
{
  calculate();
  return Item_field::val_bool_result();
}

bool Item_default_value::is_null_result()
{
  calculate();
  return Item_field::is_null_result();
}

my_decimal *Item_default_value::val_decimal_result(my_decimal *decimal_value)
{
  calculate();
  return Item_field::val_decimal_result(decimal_value);
}

bool Item_default_value::get_date_result(THD *thd, MYSQL_TIME *ltime,
                                         date_mode_t fuzzydate)
{
  calculate();
  return Item_field::get_date_result(thd, ltime, fuzzydate);
}

bool Item_default_value::val_native_result(THD *thd, Native *to)
{
  calculate();
  return Item_field::val_native_result(thd, to);
}


table_map Item_default_value::used_tables() const
{
  if (!field || !field->default_value)
    return static_cast<table_map>(0);
  if (!field->default_value->expr)           // not fully parsed field
    return static_cast<table_map>(RAND_TABLE_BIT);
  return field->default_value->expr->used_tables();
}

bool Item_default_value::register_field_in_read_map(void *arg)
{
  TABLE *table= (TABLE *) arg;
  int res= 0;
  if (!table || (table && table == field->table))
  {
    if (field->default_value && field->default_value->expr)
      res= field->default_value->expr->walk(&Item::register_field_in_read_map,1,arg);
  }
  else if (result_field && table == result_field->table)
  {
    bitmap_set_bit(table->read_set, result_field->field_index);
  }

  return res;
}

/**
  This method like the walk method traverses the item tree, but at the
  same time it can replace some nodes in the tree.
*/ 

Item *Item_default_value::transform(THD *thd, Item_transformer transformer,
                                    uchar *args)
{
  DBUG_ASSERT(!thd->stmt_arena->is_stmt_prepare());
  DBUG_ASSERT(arg);

  Item *new_item= arg->transform(thd, transformer, args);
  if (!new_item)
    return 0;

  /*
    THD::change_item_tree() should be called only if the tree was
    really transformed, i.e. when a new item has been created.
    Otherwise we'll be allocating a lot of unnecessary memory for
    change records at each execution.
  */
  if (arg != new_item)
    thd->change_item_tree(&arg, new_item);
  return (this->*transformer)(thd, args);
}


bool Item_insert_value::eq(const Item *item, bool binary_cmp) const
{
  return item->type() == INSERT_VALUE_ITEM &&
    ((Item_insert_value *)item)->arg->eq(arg, binary_cmp);
}


bool Item_insert_value::fix_fields(THD *thd, Item **items)
{
  DBUG_ASSERT(fixed() == 0);
  /* We should only check that arg is in first table */
  if (!arg->fixed())
  {
    bool res;
    TABLE_LIST *orig_next_table= context->last_name_resolution_table;
    context->last_name_resolution_table= context->first_name_resolution_table;
    res= arg->fix_fields(thd, &arg);
    context->last_name_resolution_table= orig_next_table;
    if (res)
      return TRUE;
  }

  if (arg->type() == REF_ITEM)
    arg= static_cast<Item_ref *>(arg)->ref[0];
  if (unlikely(arg->type() != FIELD_ITEM))
  {
    my_error(ER_BAD_FIELD_ERROR, MYF(0), "", "VALUES() function");
    return TRUE;
  }

  Item_field *field_arg= (Item_field *)arg;

  if (field_arg->field->table->insert_values)
  {
    Field *def_field= (Field*) thd->alloc(field_arg->field->size_of());
    if (!def_field)
      return TRUE;
    memcpy((void *)def_field, (void *)field_arg->field,
           field_arg->field->size_of());
    def_field->move_field_offset((my_ptrdiff_t)
                                 (def_field->table->insert_values -
                                  def_field->table->record[0]));
    set_field(def_field);
  }
  else
  {
    static uchar null_bit=1;
    /* charset doesn't matter here */
    Field *tmp_field= new Field_string(0, 0, &null_bit, 1, Field::NONE,
                                &field_arg->field->field_name, &my_charset_bin);
    if (tmp_field)
    {
      tmp_field->init(field_arg->field->table);
      set_field(tmp_field);
      // the index is important when read bits set
      tmp_field->field_index= field_arg->field->field_index;
    }
  }
  return FALSE;
}

void Item_insert_value::print(String *str, enum_query_type query_type)
{
  str->append(STRING_WITH_LEN("value("));
  arg->print(str, query_type);
  str->append(')');
}


/**
  Find index of Field object which will be appropriate for item
  representing field of row being changed in trigger.

  @param thd     current thread context
  @param table   table of trigger (and where we looking for fields)
  @param table_grant_info   GRANT_INFO of the subject table

  @note
    This function does almost the same as fix_fields() for Item_field
    but is invoked right after trigger definition parsing. Since at
    this stage we can't say exactly what Field object (corresponding
    to TABLE::record[0] or TABLE::record[1]) should be bound to this
    Item, we only find out index of the Field and then select concrete
    Field object in fix_fields() (by that time Table_triggers_list::old_field/
    new_field should point to proper array of Fields).
    It also binds Item_trigger_field to Table_triggers_list object for
    table of trigger which uses this item.
*/

void Item_trigger_field::setup_field(THD *thd, TABLE *table,
                                     GRANT_INFO *table_grant_info)
{
  /*
    It is too early to mark fields used here, because before execution
    of statement that will invoke trigger other statements may use same
    TABLE object, so all such mark-up will be wiped out.
    So instead we do it in Table_triggers_list::mark_fields_used()
    method which is called during execution of these statements.
  */
  enum_column_usage saved_column_usage= thd->column_usage;
  thd->column_usage= want_privilege == SELECT_ACL ? COLUMNS_READ
                                                  : COLUMNS_WRITE;
  /*
    Try to find field by its name and if it will be found
    set field_idx properly.
  */
  (void)find_field_in_table(thd, table, field_name.str, field_name.length,
                            0, &field_idx);
  thd->column_usage= saved_column_usage;
  triggers= table->triggers;
  table_grants= table_grant_info;
}


bool Item_trigger_field::eq(const Item *item, bool binary_cmp) const
{
  return item->type() == TRIGGER_FIELD_ITEM &&
         row_version == ((Item_trigger_field *)item)->row_version &&
         !lex_string_cmp(system_charset_info, &field_name,
                         &((Item_trigger_field *)item)->field_name);
}


void Item_trigger_field::set_required_privilege(bool rw)
{
  /*
    Require SELECT and UPDATE privilege if this field will be read and
    set, and only UPDATE privilege for setting the field.
  */
  want_privilege= (rw ? SELECT_ACL | UPDATE_ACL : UPDATE_ACL);
}


bool Item_trigger_field::set_value(THD *thd, sp_rcontext * /*ctx*/, Item **it)
{
  Item *item= thd->sp_prepare_func_item(it);

  if (!item || fix_fields_if_needed(thd, NULL))
    return true;

  // NOTE: field->table->copy_blobs should be false here, but let's
  // remember the value at runtime to avoid subtle bugs.
  bool copy_blobs_saved= field->table->copy_blobs;

  field->table->copy_blobs= true;

  int err_code= item->save_in_field(field, 0);

  field->table->copy_blobs= copy_blobs_saved;
  field->set_has_explicit_value();

  return err_code < 0;
}


bool Item_trigger_field::fix_fields(THD *thd, Item **items)
{
  /*
    Since trigger is object tightly associated with TABLE object most
    of its set up can be performed during trigger loading i.e. trigger
    parsing! So we have little to do in fix_fields. :)
  */

  DBUG_ASSERT(fixed() == 0);

  /* Set field. */

  if (likely(field_idx != NO_CACHED_FIELD_INDEX))
  {
#ifndef NO_EMBEDDED_ACCESS_CHECKS
    /*
      Check access privileges for the subject table. We check privileges only
      in runtime.
    */

    if (table_grants)
    {
      table_grants->want_privilege= want_privilege;

      if (check_grant_column(thd, table_grants,
                             triggers->trigger_table->s->db.str,
                             triggers->trigger_table->s->table_name.str,
                             field_name.str, field_name.length,
                             thd->security_ctx))
        return TRUE;
    }
#endif // NO_EMBEDDED_ACCESS_CHECKS

    field= (row_version == OLD_ROW) ? triggers->old_field[field_idx] :
                                      triggers->new_field[field_idx];
    set_field(field);
    base_flags|= item_base_t::FIXED;
    return FALSE;
  }

  my_error(ER_BAD_FIELD_ERROR, MYF(0), field_name.str,
           (row_version == NEW_ROW) ? "NEW" : "OLD");
  return TRUE;
}


void Item_trigger_field::print(String *str, enum_query_type query_type)
{
  str->append((row_version == NEW_ROW) ? "NEW" : "OLD", 3);
  str->append('.');
  str->append(&field_name);
}


bool Item_trigger_field::check_vcol_func_processor(void *arg)
{
  const char *ver= row_version == NEW_ROW ? "NEW." : "OLD.";
  return mark_unsupported_function(ver, field_name.str, arg, VCOL_IMPOSSIBLE);
}


void Item_trigger_field::cleanup()
{
  want_privilege= original_privilege;
  /*
    Since special nature of Item_trigger_field we should not do most of
    things from Item_field::cleanup() or Item_ident::cleanup() here.
  */
  Item_fixed_hybrid::cleanup();
}


Item_result item_cmp_type(Item_result a,Item_result b)
{
  if (a == b)
    return a;
  if (a == ROW_RESULT || b == ROW_RESULT)
    return ROW_RESULT;
  if (a == TIME_RESULT || b == TIME_RESULT)
    return TIME_RESULT;
  if ((a == INT_RESULT || a == DECIMAL_RESULT) &&
      (b == INT_RESULT || b == DECIMAL_RESULT))
    return DECIMAL_RESULT;
  return REAL_RESULT;
}


void resolve_const_item(THD *thd, Item **ref, Item *comp_item)
{
  Item *item= *ref;
  if (item->basic_const_item())
    return;                                     // Can't be better
  Type_handler_hybrid_field_type cmp(comp_item->type_handler_for_comparison());
  if (!cmp.aggregate_for_comparison(item->type_handler_for_comparison()))
  {
    Item *new_item= cmp.type_handler()->
                     make_const_item_for_comparison(thd, item, comp_item);
    if (new_item)
      thd->change_item_tree(ref, new_item);
  }
}

/**
  Compare the value stored in field with the expression from the query.

  @param field   Field which the Item is stored in after conversion
  @param item    Original expression from query

  @return Returns an integer greater than, equal to, or less than 0 if
          the value stored in the field is greater than, equal to,
          or less than the original Item. A 0 may also be returned if 
          out of memory.          

  @note We use this in the range optimizer/partition pruning,
        because in some cases we can't store the value in the field
        without some precision/character loss.

        We similarly use it to verify that expressions like
        BIGINT_FIELD <cmp> <literal value>
        is done correctly (as int/decimal/float according to literal type).

  @todo rewrite it to use Arg_comparator (currently it's a simplified and
        incomplete version of it)
*/

int stored_field_cmp_to_item(THD *thd, Field *field, Item *item)
{
  Type_handler_hybrid_field_type cmp(field->type_handler_for_comparison());
  if (cmp.aggregate_for_comparison(item->type_handler_for_comparison()))
  {
    // At fix_fields() time we checked that "field" and "item" are comparable
    DBUG_ASSERT(0);
    return 0;
  }
  return cmp.type_handler()->stored_field_cmp_to_item(thd, field, item);
}


void Item_cache::store(Item *item)
{
  example= item;
  if (!item)
    null_value= TRUE;
  value_cached= FALSE;
}

void Item_cache::print(String *str, enum_query_type query_type)
{
  if (example &&                                 // There is a cached item
      (query_type & QT_NO_DATA_EXPANSION))       // Caller is show-create-table
  {
    // Instead of "cache" or the cached value, print the cached item name
    example->print(str, query_type);
    return;
  }

  if (value_cached)
  {
    print_value(str);
    return;
  }
  str->append(STRING_WITH_LEN("<cache>("));
  if (example)
    example->print(str, query_type);
  else
    Item::print(str, query_type);
  str->append(')');
}

/**
  Assign to this cache NULL value if it is possible
*/

void Item_cache::set_null()
{
  if (maybe_null())
  {
    null_value= TRUE;
    value_cached= TRUE;
  }
}


bool  Item_cache_int::cache_value()
{
  if (!example)
    return FALSE;
  value_cached= TRUE;
  value= example->val_int_result();
  null_value_inside= null_value= example->null_value;
  unsigned_flag= example->unsigned_flag;
  return TRUE;
}


String *Item_cache_int::val_str(String *str)
{
  if (!has_value())
    return NULL;
  str->set_int(value, unsigned_flag, default_charset());
  return str;
}


my_decimal *Item_cache_int::val_decimal(my_decimal *decimal_val)
{
  if (!has_value())
    return NULL;
  int2my_decimal(E_DEC_FATAL_ERROR, value, unsigned_flag, decimal_val);
  return decimal_val;
}

double Item_cache_int::val_real()
{
  if (!has_value())
    return 0.0;
  return (double) value;
}

longlong Item_cache_int::val_int()
{
  if (!has_value())
    return 0;
  return value;
}

int Item_cache_int::save_in_field(Field *field, bool no_conversions)
{
  int error;
  if (!has_value())
    return set_field_to_null_with_conversions(field, no_conversions);

  field->set_notnull();
  error= field->store(value, unsigned_flag);

  return error ? error : field->table->in_use->is_error() ? 1 : 0;
}


Item *Item_cache_int::convert_to_basic_const_item(THD *thd)
{
  Item *new_item;
  DBUG_ASSERT(value_cached || example != 0);
  if (!value_cached)
    cache_value();
  new_item= null_value ?
            (Item*) new (thd->mem_root) Item_null(thd) :
	    (Item*) new (thd->mem_root) Item_int(thd, val_int(), max_length);
  return new_item;
} 


Item_cache_temporal::Item_cache_temporal(THD *thd, const Type_handler *handler)
 :Item_cache_int(thd, handler)
{
  if (mysql_timestamp_type() == MYSQL_TIMESTAMP_ERROR)
    set_handler(&type_handler_datetime2);
}


bool Item_cache_temporal::cache_value()
{
  if (!example)
    return false;
  value_cached= true;
  value= example->val_datetime_packed_result(current_thd);
  null_value_inside= null_value= example->null_value;
  return true;
}


bool Item_cache_time::cache_value()
{
  if (!example)
    return false;
  value_cached= true;
  value= example->val_time_packed_result(current_thd);
  null_value_inside= null_value= example->null_value;
  return true;
}


bool Item_cache_temporal::get_date(THD *thd, MYSQL_TIME *ltime, date_mode_t fuzzydate)
{
  if (!has_value())
  {
    bzero((char*) ltime,sizeof(*ltime));
    return (null_value= true);
  }

  unpack_time(value, ltime, mysql_timestamp_type());
  return 0;
}


int Item_cache_temporal::save_in_field(Field *field, bool no_conversions)
{
  MYSQL_TIME ltime;
  // This is a temporal type. No nanoseconds, so round mode is not important.
  if (get_date(field->get_thd(), &ltime, TIME_CONV_NONE | TIME_FRAC_NONE))
    return set_field_to_null_with_conversions(field, no_conversions);
  field->set_notnull();
  int error= field->store_time_dec(&ltime, decimals);
  return error ? error : field->table->in_use->is_error() ? 1 : 0;
}


void Item_cache_temporal::store_packed(longlong val_arg, Item *example_arg)
{
  /* An explicit value is given, save it. */
  store(example_arg);
  value_cached= true;
  value= val_arg;
  null_value= false;
}


Item *Item_cache_temporal::clone_item(THD *thd)
{
  Item_cache *tmp= type_handler()->Item_get_cache(thd, this);
  Item_cache_temporal *item= static_cast<Item_cache_temporal*>(tmp);
  item->store_packed(value, example);
  return item;
}


Item *Item_cache_temporal::convert_to_basic_const_item(THD *thd)
{
  DBUG_ASSERT(value_cached || example != 0);
  if (!value_cached)
    cache_value();
  if (null_value)
    return new (thd->mem_root) Item_null(thd);
  return make_literal(thd);
}

Item *Item_cache_datetime::make_literal(THD *thd)
{
  Datetime dt(thd, this, TIME_CONV_NONE | TIME_FRAC_NONE);
  return new (thd->mem_root) Item_datetime_literal(thd, &dt, decimals);
}

Item *Item_cache_date::make_literal(THD *thd)
{
  Date d(thd, this, TIME_CONV_NONE | TIME_FRAC_NONE);
  return new (thd->mem_root) Item_date_literal(thd, &d);
}

Item *Item_cache_time::make_literal(THD *thd)
{
  Time t(thd, this);
  return new (thd->mem_root) Item_time_literal(thd, &t, decimals);
}


int Item_cache_timestamp::save_in_field(Field *field, bool no_conversions)
{
  if (!has_value())
    return set_field_to_null_with_conversions(field, no_conversions);
  return m_native.save_in_field(field, decimals);
}


bool Item_cache_timestamp::val_native(THD *thd, Native *to)
{
  if (!has_value())
  {
    null_value= true;
    return true;
  }
  return (null_value= to->copy(m_native));
}


Datetime Item_cache_timestamp::to_datetime(THD *thd)
{
  DBUG_ASSERT(fixed() == 1);
  if (!has_value())
  {
    null_value= true;
    return Datetime();
  }
  return m_native.to_datetime(thd);
}


bool Item_cache_timestamp::get_date(THD *thd, MYSQL_TIME *ltime,
                                    date_mode_t fuzzydate)
{
  if (!has_value())
  {
    set_zero_time(ltime, MYSQL_TIMESTAMP_DATETIME);
    return true;
  }
  Timestamp_or_zero_datetime tm(m_native);
  return (null_value= tm.to_TIME(thd, ltime, fuzzydate));
}


bool Item_cache_timestamp::cache_value()
{
  if (!example)
    return false;
  value_cached= true;
  null_value= example->val_native_with_conversion_result(current_thd, &m_native,
                                                         type_handler());
  return true;
}


bool Item_cache_real::cache_value()
{
  if (!example)
    return FALSE;
  value_cached= TRUE;
  value= example->val_result();
  null_value_inside= null_value= example->null_value;
  return TRUE;
}


double Item_cache_real::val_real()
{
  if (!has_value())
    return 0.0;
  return value;
}

longlong Item_cache_real::val_int()
{
  if (!has_value())
    return 0;
  return Converter_double_to_longlong(value, unsigned_flag).result();
}


String* Item_cache_double::val_str(String *str)
{
  if (!has_value())
    return NULL;
  str->set_real(value, decimals, default_charset());
  return str;
}


String* Item_cache_float::val_str(String *str)
{
  if (!has_value())
    return NULL;
  Float(value).to_string(str, decimals);
  return str;
}


my_decimal *Item_cache_real::val_decimal(my_decimal *decimal_val)
{
  if (!has_value())
    return NULL;
  double2my_decimal(E_DEC_FATAL_ERROR, value, decimal_val);
  return decimal_val;
}


Item *Item_cache_real::convert_to_basic_const_item(THD *thd)
{
  Item *new_item;
  DBUG_ASSERT(value_cached || example != 0);
  if (!value_cached)
    cache_value();
  new_item= null_value ?
            (Item*) new (thd->mem_root) Item_null(thd) :
	    (Item*) new (thd->mem_root) Item_float(thd, val_real(),
                                                   decimals);
  return new_item;
} 


bool Item_cache_decimal::cache_value()
{
  if (!example)
    return FALSE;
  value_cached= TRUE;
  my_decimal *val= example->val_decimal_result(&decimal_value);
  if (!(null_value_inside= null_value= example->null_value) &&
        val != &decimal_value)
    my_decimal2decimal(val, &decimal_value);
  return TRUE;
}

double Item_cache_decimal::val_real()
{
  return !has_value() ? 0.0 : decimal_value.to_double();
}

longlong Item_cache_decimal::val_int()
{
  return !has_value() ? 0 : decimal_value.to_longlong(unsigned_flag);
}

String* Item_cache_decimal::val_str(String *str)
{
  return !has_value() ? NULL :
         decimal_value.to_string_round(str, decimals, &decimal_value);
}

my_decimal *Item_cache_decimal::val_decimal(my_decimal *val)
{
  if (!has_value())
    return NULL;
  return &decimal_value;
}


Item *Item_cache_decimal::convert_to_basic_const_item(THD *thd)
{
  Item *new_item;
  DBUG_ASSERT(value_cached || example != 0);
  if (!value_cached)
    cache_value();
  if (null_value)
    new_item= (Item*) new (thd->mem_root) Item_null(thd);
  else
  {
     VDec tmp(this);
     new_item= (Item*) new (thd->mem_root) Item_decimal(thd, tmp.ptr());
  }
  return new_item;
} 


bool Item_cache_str::cache_value()
{
  if (!example)
  {
    DBUG_ASSERT(value_cached == FALSE);
    return FALSE;
  }
  value_cached= TRUE;
  value_buff.set(buffer, sizeof(buffer), example->collation.collation);
  value= example->str_result(&value_buff);
  if ((null_value= null_value_inside= example->null_value))
    value= 0;
  else if (value != &value_buff)
  {
    /*
      We copy string value to avoid changing value if 'item' is table field
      in queries like following (where t1.c is varchar):
      select a, 
             (select a,b,c from t1 where t1.a=t2.a) = ROW(a,2,'a'),
             (select c from t1 where a=t2.a)
        from t2;
    */
    value_buff.copy(*value);
    value= &value_buff;
  }
  else
    value_buff.copy();
  return TRUE;
}

double Item_cache_str::val_real()
{
  if (!has_value())
    return 0.0;
  return value ? double_from_string_with_check(value) :  0.0;
}


longlong Item_cache_str::val_int()
{
  if (!has_value())
    return 0;
  return value ? longlong_from_string_with_check(value) : 0;
}


String* Item_cache_str::val_str(String *str)
{
  if (!has_value())
    return 0;
  return value;
}


my_decimal *Item_cache_str::val_decimal(my_decimal *decimal_val)
{
  if (!has_value())
    return NULL;
  return value ? decimal_from_string_with_check(decimal_val, value) : 0;
}


int Item_cache_str::save_in_field(Field *field, bool no_conversions)
{
  if (!has_value())
    return set_field_to_null_with_conversions(field, no_conversions);
  int res= Item_cache::save_in_field(field, no_conversions);
  return (is_varbinary && field->type() == MYSQL_TYPE_STRING &&
          value->length() < field->field_length) ? 1 : res;
}


bool Item_cache_row::allocate(THD *thd, uint num)
{
  item_count= num;
  return (!(values= 
	    (Item_cache **) thd->calloc(sizeof(Item_cache *)*item_count)));
}


Item *Item_cache_str::convert_to_basic_const_item(THD *thd)
{
  Item *new_item;
  DBUG_ASSERT(value_cached || example != 0);
  if (!value_cached)
    cache_value();
  if (null_value)
    new_item= (Item*) new (thd->mem_root) Item_null(thd);
  else
  {
    char buff[MAX_FIELD_WIDTH];
    String tmp(buff, sizeof(buff), value->charset());
    String *result= val_str(&tmp);
    uint length= result->length();
    char *tmp_str= thd->strmake(result->ptr(), length);
    new_item= new (thd->mem_root) Item_string(thd, tmp_str, length,
                                              result->charset());
  }
  return new_item;
}


bool Item_cache_row::setup(THD *thd, Item *item)
{
  example= item;
  null_value= true;

  if (!values && allocate(thd, item->cols()))
    return 1;
  for (uint i= 0; i < item_count; i++)
  {
    Item_cache *tmp;
    Item *el= item->element_index(i);
    if (!(tmp= values[i]= el->get_cache(thd)))
      return 1;
    tmp->setup(thd, el);
  }
  return 0;
}


void Item_cache_row::store(Item * item)
{
  example= item;
  if (!item)
  {
    null_value= TRUE;
    return;
  }
  for (uint i= 0; i < item_count; i++)
    values[i]->store(item->element_index(i));
}


bool Item_cache_row::cache_value()
{
  if (!example)
    return FALSE;
  value_cached= TRUE;
  null_value= TRUE;
  null_value_inside= false;
  example->bring_value();

  /*
    For Item_cache_row null_value is set to TRUE only when ALL the values
    inside the cache are NULL
  */
  for (uint i= 0; i < item_count; i++)
  {
    values[i]->cache_value();
    null_value&= values[i]->null_value;
    null_value_inside|= values[i]->null_value;
  }
  return TRUE;
}


void Item_cache_row::illegal_method_call(const char *method)
{
  DBUG_ENTER("Item_cache_row::illegal_method_call");
  DBUG_PRINT("error", ("!!! %s method was called for row item", method));
  DBUG_ASSERT(0);
  my_error(ER_OPERAND_COLUMNS, MYF(0), 1);
  DBUG_VOID_RETURN;
}


bool Item_cache_row::check_cols(uint c)
{
  if (c != item_count)
  {
    my_error(ER_OPERAND_COLUMNS, MYF(0), c);
    return 1;
  }
  return 0;
}


bool Item_cache_row::null_inside()
{
  for (uint i= 0; i < item_count; i++)
  {
    if (values[i]->cols() > 1)
    {
      if (values[i]->null_inside())
	return 1;
    }
    else
    {
      values[i]->update_null_value();
      if (values[i]->null_value)
	return 1;
    }
  }
  return 0;
}


void Item_cache_row::bring_value()
{
  if (!example)
    return;
  example->bring_value();
  null_value= example->null_value;
  for (uint i= 0; i < item_count; i++)
    values[i]->bring_value();
}


/**
  Assign to this cache NULL value if it is possible
*/

void Item_cache_row::set_null()
{
  Item_cache::set_null();
  if (!values)
    return;
  for (uint i= 0; i < item_count; i++)
    values[i]->set_null();
};


double Item_type_holder::val_real()
{
  DBUG_ASSERT(0); // should never be called
  return 0.0;
}


longlong Item_type_holder::val_int()
{
  DBUG_ASSERT(0); // should never be called
  return 0;
}

my_decimal *Item_type_holder::val_decimal(my_decimal *)
{
  DBUG_ASSERT(0); // should never be called
  return 0;
}

String *Item_type_holder::val_str(String*)
{
  DBUG_ASSERT(0); // should never be called
  return 0;
}

bool Item_type_holder::get_date(THD *thd, MYSQL_TIME *ltime, date_mode_t fuzzydate)
{
  DBUG_ASSERT(0); // should never be called
  return true;
}

void Item_result_field::cleanup()
{
  DBUG_ENTER("Item_result_field::cleanup()");
  Item_fixed_hybrid::cleanup();
  result_field= 0;
  DBUG_VOID_RETURN;
}

/**
  Dummy error processor used by default by Name_resolution_context.

  @note
    do nothing
*/

void dummy_error_processor(THD *thd, void *data)
{}

/**
  Wrapper of hide_view_error call for Name_resolution_context error
  processor.

  @note
    hide view underlying tables details in error messages
*/

void view_error_processor(THD *thd, void *data)
{
  ((TABLE_LIST *)data)->hide_view_error(thd);
}


st_select_lex *Item_ident::get_depended_from() const
{
  st_select_lex *dep;
  if ((dep= depended_from))
    for ( ; dep->merged_into; dep= dep->merged_into) ;
  return dep;
}


table_map Item_ref::used_tables() const		
{
  return get_depended_from() ? OUTER_REF_TABLE_BIT : (*ref)->used_tables(); 
}


void Item_ref::update_used_tables()
{
  if (!get_depended_from())
    (*ref)->update_used_tables();
}

void Item_direct_view_ref::update_used_tables()
{
  set_null_ref_table();
  Item_direct_ref::update_used_tables();
}


table_map Item_direct_view_ref::used_tables() const
{
  DBUG_ASSERT(fixed());

  if (get_depended_from())
    return OUTER_REF_TABLE_BIT;

  if (view->is_merged_derived() || view->merged || !view->table)
  {
    table_map used= (*ref)->used_tables();
    return (used ?
            used :
            ((null_ref_table != NO_NULL_TABLE) ?
             null_ref_table->map :
             (table_map)0 ));
  }
  return view->table->map;
}

table_map Item_direct_view_ref::not_null_tables() const
{
  if (get_depended_from())
    return 0;
  if  (!( view->merged || !view->table))
    return view->table->map;
  TABLE *tab= get_null_ref_table();
  if (tab == NO_NULL_TABLE || (*ref)->used_tables())
    return (*ref)->not_null_tables();
   return get_null_ref_table()->map;
}

/*
  we add RAND_TABLE_BIT to prevent moving this item from HAVING to WHERE
*/
table_map Item_ref_null_helper::used_tables() const
{
  return (get_depended_from() ?
          OUTER_REF_TABLE_BIT :
          (*ref)->used_tables() | RAND_TABLE_BIT);
}


#ifndef DBUG_OFF

/* Debugger help function */
static char dbug_item_print_buf[2048];

const char *dbug_print_item(Item *item)
{
  char *buf= dbug_item_print_buf;
  String str(buf, sizeof(dbug_item_print_buf), &my_charset_bin);
  str.length(0);
  if (!item)
    return "(Item*)NULL";
  
  THD *thd= current_thd;
  ulonglong save_option_bits= thd->variables.option_bits;
  thd->variables.option_bits &= ~OPTION_QUOTE_SHOW_CREATE;

  item->print(&str, QT_EXPLAIN);

  thd->variables.option_bits= save_option_bits;

  if (str.c_ptr_safe() == buf)
    return buf;
  else
    return "Couldn't fit into buffer";
}

const char *dbug_print_select(SELECT_LEX *sl)
{
  char *buf= dbug_item_print_buf;
  String str(buf, sizeof(dbug_item_print_buf), &my_charset_bin);
  str.length(0);
  if (!sl)
    return "(SELECT_LEX*)NULL";

  THD *thd= current_thd;
  ulonglong save_option_bits= thd->variables.option_bits;
  thd->variables.option_bits &= ~OPTION_QUOTE_SHOW_CREATE;

  sl->print(thd, &str, QT_EXPLAIN);

  thd->variables.option_bits= save_option_bits;

  if (str.c_ptr() == buf)
    return buf;
  else
    return "Couldn't fit into buffer";
}

const char *dbug_print_unit(SELECT_LEX_UNIT *un)
{
  char *buf= dbug_item_print_buf;
  String str(buf, sizeof(dbug_item_print_buf), &my_charset_bin);
  str.length(0);
  if (!un)
    return "(SELECT_LEX_UNIT*)NULL";

  THD *thd= current_thd;
  ulonglong save_option_bits= thd->variables.option_bits;
  thd->variables.option_bits &= ~OPTION_QUOTE_SHOW_CREATE;

  un->print(&str, QT_EXPLAIN);

  thd->variables.option_bits= save_option_bits;

  if (str.c_ptr() == buf)
    return buf;
  else
    return "Couldn't fit into buffer";
}

const char *dbug_print(Item *x)            { return dbug_print_item(x);   }
const char *dbug_print(SELECT_LEX *x)      { return dbug_print_select(x); }
const char *dbug_print(SELECT_LEX_UNIT *x) { return dbug_print_unit(x);   }

#endif /*DBUG_OFF*/



void Item::register_in(THD *thd)
{
  next= thd->free_list;
  thd->free_list= this;
}


bool Item::cleanup_excluding_immutables_processor (void *arg)
{
  if (!(get_extraction_flag() == MARKER_IMMUTABLE))
    return cleanup_processor(arg);
  else
  {
    clear_extraction_flag();
    return false;
  }
}


bool ignored_list_includes_table(ignored_tables_list_t list, TABLE_LIST *tbl)
{
  if (!list)
    return false;
  List_iterator<TABLE_LIST> it(*list);
  TABLE_LIST *list_tbl;
  while ((list_tbl = it++))
  {
    if (list_tbl == tbl)
      return true;
  }
  return false;
}
