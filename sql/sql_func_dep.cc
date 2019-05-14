#include "mariadb.h"
#include "sql_base.h"
#include "sql_select.h"

/**
  @file
   Check if SELECT list, HAVING and ORDER BY fields are used in GROUP BY
   or are functionally dependent on fields used in GROUP BY.

   Let's call fields that are used in GROUP BY 'gb' fields and
   fields that are functionally dependent on 'gb' fields 'fd'
   fields. Fields that are functionally dependent on 'fd' fields
   can also be called 'fd' fields.
   Fields that are either 'gb' or 'fd' will be called 'allowed' fields.
   'Allowed' fields are allowed to be used in SELECT list, HAVING and
   ORDER BY.

   Field F2 is called functionally dependent on some other field F1
   if such a rule holds: if two values of F1 are equal (or both NULL)
   then two corresponding values of F2 are also equal or both NULL.
   F1 and F2 can also be groups of fields:
   (F11, ..., F1n) and (F21, ..., F2m).

   Functionally dependent fields can be extracted from the WHERE clause
   and ON expression equalities.
   Current implementation is limited to the following equalities:

   F2 = g(H11, ... H1n), where

   (H11, ..., H1n) are some functions of 'allowed' fields and/or 'allowed'
                   fields and/or constants.
   g               is some function. It can be identity function.
   F2              is some non-'allowed' field.

   Work if 'only_full_group_by' mode is set only.
*/


/**
  This class is used to store the equality information
  that can be used in the extraction of a new functionally
  dependent field.
  It contains a field that can be extracted from the equality (one
  part of the equality) and a set of fields used in the other part
  of the same equality.
*/

class Item_equal_fd_info :public Sql_alloc
{
public:
  Field *nd_field; // Field that can be extracted
  List<Field> dp_fields; // Set of fields from the other part of the equality
  Item_equal_fd_info(Field *nd_f, List<Field> dp_f) :
    nd_field(nd_f), dp_fields(dp_f) {}
};


/**
  This class is used to store the information about the current JOIN
  level. It is used in the extraction of a new functionally dependent field.
  Is updated every time when a new nested JOIN is entered.
*/

class FD_select_info :public Sql_alloc
{
public:
  st_select_lex *sl; // Current SELECT
  /*
    If the current JOIN level is the most outer JOIN store WHERE clause
    equalities information that can be used in extraction of new functionally
    dependent fields.
    If the current JOIN is nested JOIN store its ON expression
    equalities information.
  */
  List<Item_equal_fd_info> *eq_info;
  table_map cur_level_tabs; // Map of current JOIN level tables.
  /*
    True if it is forbidden to extract new functionally dependent fields
    from this JOIN level.
  */
  bool forbid_extraction;
  FD_select_info(st_select_lex *sel, List<Item_equal_fd_info> *eq_inf)
    : sl(sel), eq_info(eq_inf), cur_level_tabs(0),
      forbid_extraction(false) {}
};


/**
  Check if all 'key' parts are 'allowed' fields.
  If so return true.
*/

static bool are_all_key_fields_allowed(KEY *key)
{
  Item *err_item= 0;
  for (uint i= 0; i < key->user_defined_key_parts; i++)
  {
    if (!key->key_part[i].field->
         excl_func_dep_on_grouping_fields(0, 0, false, &err_item))
      return false;
  }
  return true;
}


/**
  @brief
    Check if PRIMARY or UNIQUE keys fields are 'allowed'

  @param
    sl  current select

  @details
    For each table used in the FROM list of SELECT sl check
    its PRIMARY and UNIQUE keys.
    If some table key consists of 'allowed' fields only then
    all fields of this table are 'allowed'.

  @retval
    true   if new 'allowed' fields are extracted
    false  otherwise
*/

static
bool find_allowed_unique_keys(st_select_lex *sl)
{
  List_iterator<TABLE_LIST> it(sl->leaf_tables);
  TABLE_LIST *tbl;
  bool extracted= false;
  while ((tbl= it++))
  {
    if (!tbl->table)
      continue;
    /* Check if all fields of this table are already marked as 'allowed'. */
    if (bitmap_is_set_all(&tbl->table->tmp_set))
      continue;
    /* Check if PRIMARY key fields are 'allowed'. */
    if (tbl->table->s->primary_key < MAX_KEY)
    {
      KEY *pk= &tbl->table->key_info[tbl->table->s->primary_key];
      if (are_all_key_fields_allowed(pk))
      {
        bitmap_set_all(&tbl->table->tmp_set);
        extracted= true;
        continue;
      }
    }
    /* Check if UNIQUE keys fields are 'allowed' */
    KEY *end= tbl->table->key_info + tbl->table->s->keys;
    for (KEY *k= tbl->table->key_info; k < end; k++)
      if ((k->flags & HA_NOSAME) && are_all_key_fields_allowed(k))
      {
        bitmap_set_all(&tbl->table->tmp_set);
        extracted= true;
        break;
      }
  }
  return extracted;
}


/**
  @brief
    Check if TABLE_LIST is a uniquely identified materialized derived table

  @param
    tbl  TABLE_LIST to check

  @details
    Check if tbl is a materialized derived table that is defined with a
    single SELECT and GROUP BY. If so that means that all fields of this
    derived table are uniquely identified (materialized derived tables are
    checked before they are used).
    If some field of such tbl is functionally dependent on 'gb' fields or
    is GROUP BY field (in SELECT where this tbl is used) then all fields
    of this materialized derived table can be marked as ‘allowed’ as they
    are uniquely identified in this SELECT.
    The rule doesn't hold if a derived table is on the weak side of
    some non-inner JOIN.

  @retval
    true   if the rules are followed for the derived table
    false  otherwise
*/

inline bool uniquely_identified_mat_der_tab(TABLE_LIST *tbl)
{
  return tbl->is_materialized_derived() &&
         tbl->derived_uniq_ident &&
         !tbl->on_expr;
}


/**
  @brief
    Collect fields used in GROUP BY

  @param
    sl           current select
    gb_items     list of GROUP BY non-field items

  @details
    For each table used in the FROM clause of the SELECT sl collect
    its fields used in the GROUP BY of sl.
    Mark them in tmp_set map.
    If GROUP BY item is not a field store it in gb_items list.

  @retval
    true   if an error occurs
    false  otherwise
*/

static
bool collect_gb_items(st_select_lex *sl, List<Item> &gb_items)
{
  THD *thd= sl->join->thd;
  if (!sl->group_list.elements)
    return false;

  for (ORDER *ord= sl->group_list.first; ord; ord= ord->next)
  {
    Item *ord_item= *ord->item;
    if (ord_item->type() == Item::FIELD_ITEM ||
        ord_item->real_item()->type() == Item::FIELD_ITEM)
    {
      Field *fld= ((Item_field *)(ord_item->real_item()))->field;
      if (uniquely_identified_mat_der_tab(fld->table->pos_in_table_list))
        bitmap_set_all(&fld->table->tmp_set);
      else
        bitmap_set_bit(&fld->table->tmp_set,
                       fld->field_index);
    }
    else if (gb_items.push_back(ord_item, thd->mem_root))
      return true;
  }

  /* Check if fields used in the GROUP BY are key fields. */
  if (sl->olap == UNSPECIFIED_OLAP_TYPE)
    find_allowed_unique_keys(sl);
  return false;
}


/**
  @brief
    Get equality information so it can be used in extraction of fd field

  @param
    sl_info            information about the current JOIN level
                       and SELECT
    eq                 the considered equality
    curr_dep_part_idx  index of the equality part which is a field of some
                       current JOIN level table

  @details
    Check if equality can be used in functionally dependent field extraction
    and if so collect its internal information.

    Consider equalities of the form:

    F2 = g(H11,...,H1n)      (1),

    where F2           is some field
          H11,...,H1n  are some fields, constants or functions of fields and
                       constants
          g            is some function (can be identity function)

    (2) Equality can be used if:
    1. At least one of its parts is a field of one of the current JOIN level
       tables.
       It should be F2 in (1).
    2. (H11,..,H1n) are either:
       a. a current JOIN level tables single field.
       b. set of current JOIN level tables fields or constants or functions
          that return deterministic result and use these fields or constants.
       c. some outer level JOIN tables single field
       d. constant
    3. No conversion is applied to F2 (F2 type is the same as the
       equality type).
    4. g returns deterministic result

    If the rules above hold and F2 is not marked as 'allowed' it can be
    extracted as a functionally dependent field in further processing.
    Internal equality information is saved in sl_info->eq_info list.

  @note
    Consider a special case of the equality (1) for which (2) holds:

    F2 = I(H1),

    where I is identity function.

    Let H1 be a field of some current JOIN level table with no
    conversion to equality type and it is not 'allowed' field:
    Swap equalities sides and get a new equality:

    H1 = I(F2)

    H1 can also be extracted as a functionally dependent field.
    Save this new equality information in sl_info->eq_info list.

  @note
    If considered equality is WHERE equality check if it doesn't
    use forbidden outer SELECTs fields.

  @retval
    true   if an error occurs
    false  otherwise
*/

static
bool get_eq_info_for_fd_field_extraction(FD_select_info *sl_info,
                                         Item_func_eq *eq,
                                         uint curr_dep_part_idx)
{
  Item *curr_dep_part= eq->arguments()[curr_dep_part_idx];
  DBUG_ASSERT(curr_dep_part->real_item()->type() == Item::FIELD_ITEM ||
              curr_dep_part->used_tables() & sl_info->cur_level_tabs);
  Item *op_equal_part=
    (curr_dep_part_idx == 0) ? eq->arguments()[1] : eq->arguments()[0];

  THD *thd= sl_info->sl->join->thd;
  Item *err_item= 0;
  List<Field> curr_dep_fld;
  List<Field> op_part_flds;

  bool dep_curr=
    curr_dep_part->check_usage_in_fd_field_extraction(sl_info->sl,
                                                      &curr_dep_fld,
                                                      &err_item);
  if (!dep_curr && curr_dep_fld.is_empty())
  {
    /*
      If equality can't be used in extraction of new functionally dependent
      field or a forbidden outer SELECT field is used.
    */
    if (err_item)
    {
      my_error(ER_NON_GROUPING_FIELD_USED, MYF(0),
               err_item->real_item()->full_name(), "WHERE clause");
      return true;
    }
    return false;
  }
  bool dep_on_outer= op_equal_part->used_tables() & (~sl_info->cur_level_tabs);
  /*
    Don't use equality with a special case in (1): when there are at least two
    F11 and F12 fields and at least one of them is not a current JOIN level
    table field.
  */
  if (dep_on_outer &&
      op_equal_part->real_item()->type() != Item::FIELD_ITEM)
    return false;
  bool dep_op=
    op_equal_part->check_usage_in_fd_field_extraction(sl_info->sl,
                                                      &op_part_flds,
                                                      &err_item);
  if ((!dep_op && err_item) || (dep_curr && dep_op) ||
      (op_equal_part->type() == Item::FUNC_ITEM &&
       !((Item_func *)op_equal_part)->is_deterministic))
  {
    /*
      If equality can't be used in extraction of new functionally dependent
      field or a forbidden outer SELECT field is used.
      or
      If the equality depend on 'allowed' fields only or is constant.
    */
    if (err_item)
    {
      my_error(ER_NON_GROUPING_FIELD_USED, MYF(0),
               err_item->real_item()->full_name(), "WHERE clause");
      return true;
    }
    return false;
  }
  if (!dep_curr)
  {
    if (eq->compare_type_handler() ==
        curr_dep_part->type_handler_for_comparison())
    {
      Item_equal_fd_info *new_eq=
        new (thd->mem_root) Item_equal_fd_info(
              ((Item_field *)curr_dep_part->real_item())->field,
               op_part_flds);
      if (sl_info->eq_info->push_back(new_eq, thd->mem_root))
        return true;
    }
    if (!dep_op && !dep_on_outer &&
        op_equal_part->real_item()->type() == Item::FIELD_ITEM &&
        (eq->compare_type_handler() ==
         op_equal_part->type_handler_for_comparison()))
    {
      Item_equal_fd_info *new_eq=
        new (thd->mem_root) Item_equal_fd_info(
              ((Item_field *)op_equal_part->real_item())->field,
               curr_dep_fld);
      if (sl_info->eq_info->push_back(new_eq, thd->mem_root))
        return true;
    }
  }
  else if ((op_equal_part->real_item()->type() == Item::FIELD_ITEM)
           && (!dep_on_outer) &&
           (eq->compare_type_handler() ==
            op_equal_part->type_handler_for_comparison()))
  {
    Item_equal_fd_info *new_eq=
      new (thd->mem_root) Item_equal_fd_info(
            ((Item_field *)op_equal_part->real_item())->field,
             curr_dep_fld);
    if (sl_info->eq_info->push_back(new_eq, thd->mem_root))
      return true;
  }
  return false;
}


/**
  @brief
    Check if equality can be used in fd extraction

  @param
    sl_info  information about the current JOIN level and SELECT
    eq       the considered equality
    checked  true if eq will be checked on forbidden outer SELECT
             fields in get_eq_info_for_fd_field_extraction() method
             which is called there.

  @details
    Check if at least one part of the equality has the same type
    as the equality. If so and at least one part of the equality
    is a current JOIN level table field call
    get_eq_info_for_fd_field_extraction() method.

  @retval
    true   if an error occurs
    false  otherwise
*/

static
bool check_equality_usage_in_fd_field_extraction(FD_select_info *sl_info,
                                                 Item_func_eq *eq,
                                                 bool *checked)
{
  if (eq->const_item())
    return false;

  Item *item_l= eq->arguments()[0];
  Item *item_r= eq->arguments()[1];

  if ((item_l->type_handler_for_comparison() !=
       eq->compare_type_handler()) &&
      (item_r->type_handler_for_comparison() !=
       eq->compare_type_handler()))
    return false;

  if ((item_l->used_tables() & sl_info->cur_level_tabs) &&
      (item_l->real_item()->type() == Item::FIELD_ITEM))
  {
    *checked= true;
    if (get_eq_info_for_fd_field_extraction(sl_info, eq, 0))
      return true;
  }
  else if ((item_r->used_tables() & sl_info->cur_level_tabs) &&
           (item_r->real_item()->type() == Item::FIELD_ITEM))
  {
    *checked= true;
    if (get_eq_info_for_fd_field_extraction(sl_info, eq, 1))
      return true;
  }
  return false;
}


/**
  Check if WHERE item contains forbidden outer SELECT field.
*/

static
bool check_where_on_forbidden_outer_field(st_select_lex *sl,
                                          Item *item)
{
  if (!sl->master_unit()->outer_select())
    return false;

  Item *err_item= 0;
  if (!item->excl_func_dep_on_grouping_fields(sl, 0,
                                              true, &err_item) &&
      err_item)
  {
    my_error(ER_NON_GROUPING_FIELD_USED, MYF(0),
             err_item->real_item()->full_name(), "WHERE clause");
    return true;
  }
  return false;
}


/**
  Check each upper level WHERE equality if it can be used in
  extraction of a new functionally dependent field of the current
  JOIN level table.
  For this purpose call check_equality_usage_in_fd_field_extraction().
  If needed check WHERE clause items on usage of forbidden outer SELECT
  fields.
*/

bool check_where_and_get_equalities_info(FD_select_info *sl_info)
{
  Item *cond= sl_info->sl->join->conds;
  if (!cond)
    return false;
  bool checked= false;

  if ((cond->type() == Item::COND_ITEM) &&
      ((Item_cond*) cond)->functype() == Item_func::COND_AND_FUNC)
  {
    List_iterator_fast<Item> li(*((Item_cond*) cond)->argument_list());
    Item *item;
    while ((item=li++))
    {
      checked= false;
      if (item->type() == Item::FUNC_ITEM &&
          ((Item_func_eq *) item)->functype() == Item_func::EQ_FUNC &&
          check_equality_usage_in_fd_field_extraction(sl_info,
                                                      (Item_func_eq *)item,
                                                      &checked))
          return true;
      else if (!checked &&
               check_where_on_forbidden_outer_field(sl_info->sl, item))
        return true;
    }
  }
  else if (cond->type() == Item::FUNC_ITEM &&
           ((Item_func*) cond)->functype() == Item_func::EQ_FUNC &&
           check_equality_usage_in_fd_field_extraction(sl_info,
                                                       (Item_func_eq *)cond,
                                                       &checked))
    return true;
  else if (!checked &&
           check_where_on_forbidden_outer_field(sl_info->sl, cond))
    return true;
  return false;
}


/**
  If the considered ON expression item depends on outer table fields
  which are not 'allowed' forbid extraction of a new functionally
  dependent field of some current JOIN level table.
  Otherwise check equality if it can be used in extraction of a new
  functionally dependent field in
  check_equality_usage_in_fd_field_extraction() method.
*/

bool check_on_expr_item_usage_in_fd_field_extraction(FD_select_info *sl_info,
                                                     Item *item)
{
  if (!sl_info->forbid_extraction &&
      (item->used_tables() & ~(sl_info->cur_level_tabs)))
  {
    if (item->walk(&Item::has_outer_nogb_field_processor, 0,
                     &sl_info->cur_level_tabs))
      sl_info->forbid_extraction= true;
  }
  if (item->type() == Item::FUNC_ITEM &&
      ((Item_func_eq *) item)->functype() == Item_func::EQ_FUNC)
  {
    bool checked= false;
    if (check_equality_usage_in_fd_field_extraction(sl_info,
          (Item_func_eq *)item, &checked))
      return true;
  }
  return false;
}


/**
  Check each upper level ON expression equality if it can be used in
  the extraction of a new functionally dependent field of the current
  JOIN level table.
  For this purpose call check_equality_usage_in_fd_field_extraction().
  If ON expression contains random function don't allow to use
  any equality of this ON expression.
*/

bool get_on_expr_equalities_info(FD_select_info *sl_info, Item *on_expr)
{
  if (!on_expr)
    return false;

  if ((on_expr->type() == Item::COND_ITEM ||
      on_expr->type() == Item::FUNC_ITEM) &&
      ((Item_func *)on_expr)->has_rand_bit())
    return false;

  if (on_expr->type() == Item::COND_ITEM &&
      ((Item_cond*) on_expr)->functype() == Item_func::COND_AND_FUNC)
  {
    List_iterator_fast<Item> li(*((Item_cond*) on_expr)->argument_list());
    Item *item;
    while ((item=li++))
    {
      if (check_on_expr_item_usage_in_fd_field_extraction(sl_info, item))
        return true;
    }
  }
  else if (check_on_expr_item_usage_in_fd_field_extraction(sl_info, on_expr))
    return true;

  return false;
}


/**
  @brief
    Try to extract new functionally dependent fields

  @param
    sl_info  information about the current JOIN level and SELECT

  @details
    Go through the equalities information gathered before and try to
    extract new functionally dependent fields of current JOIN level
    tables. Stop if no fields were extracted on the previous step
    or all fields are already extracted.

    Consider equality:

    F1 = g(H11,..,H1n)

    Field F1 can be extracted from the equality if the right part of
    the equality depends on 'allowed' fields and constants only.

  @note
    If sl_info->forbid_extraction is set and no F1 table fields are used
    in GROUP BY F1 can't be extracted.
*/

static
void get_new_dependencies_from_eq_info(FD_select_info *sl_info)
{
  List<Item_equal_fd_info> *eq_info= sl_info->eq_info;
  /*
    Nothing to extract from.
  */
  if (eq_info->is_empty())
    return;

  List_iterator<Item_equal_fd_info> li(*eq_info);
  Item_equal_fd_info *info;
  Item *err_item= 0;
  bool extracted= true;

  while (extracted && !eq_info->is_empty())
  {
    extracted= false;
    li.rewind();
    while ((info= li++))
    {
      if (bitmap_is_set(&info->nd_field->table->tmp_set,
                        info->nd_field->field_index))
      {
        /*
          Field is already found as 'allowed'.
        */
        li.remove();
        continue;
      }
      /*
        If sl_info->forbid_extraction is set and no F1 table fields
        are used in GROUP BY F1 can't be extracted.
      */
      if (sl_info->forbid_extraction &&
          bitmap_is_clear_all(&info->nd_field->table->tmp_set))
        continue;
      List_iterator_fast<Field> it(info->dp_fields);
      Field *fld;
      bool dep= true;

      while ((fld= it++))
        dep&= fld->excl_func_dep_on_grouping_fields(sl_info->sl, 0,
                                                    false, &err_item);
      if (!dep)
        continue;

      fld= info->nd_field;
      /* Mark nd_field as 'allowed' */
      bitmap_set_bit(&fld->table->tmp_set, fld->field_index);
      if (uniquely_identified_mat_der_tab(fld->table->pos_in_table_list))
        bitmap_set_all(&fld->table->tmp_set);
      extracted= true;
      li.remove();
    }
    if (!extracted || eq_info->is_empty())
    {
      /* Check if any keys fields become 'allowed'. */
      if (find_allowed_unique_keys(sl_info->sl))
        extracted= true;
    }
  }
}


/**
  @brief
    Recursively extract fd fields of nested JOINs tables

  @param
    sl_info    information about the current JOIN level and SELECT
    tabs_list  current JOIN level tables list
    on_expr    current JOIN level ON expression

  @details
    Collect this JOIN level tables and try to extract new functionally
    dependent fields of these tables using ON expression equalities,
    GROUP BY fields of these tables, constants and GROUP BY or 'fd'
    fields of the outer JOIN tables that were extracted before.

    If there is a table in the weak part of this JOIN try to extract its fields
    using ON expression of the considered JOIN.
    Otherwise, if the weak part of this JOIN contains nested join recursively
    call this method for this nested join.

    It can be forbidden to extract new functionally dependent fields
    of this JOIN level tables.
    It can be so if the considered JOIN is the LEFT JOIN (it's weak part)
    and no fields from the other part of the JOIN are used in ON expression.

  @note
    Information about the considered JOIN level is stored in sl_info
    fields.

  @retval
    true   if an error occurs
    false  otherwise
*/

bool extract_table_list_func_dep_fields(FD_select_info *sl_info,
                                        List<TABLE_LIST> *tabs_list,
                                        Item *on_expr)
{
  List<TABLE_LIST> dep_tabs;
  table_map cur_level_tabs= 0;
  List_iterator_fast<TABLE_LIST> it(*tabs_list);
  TABLE_LIST *tbl;
  while ((tbl= it++))
  {
    if (tbl->table && !tbl->on_expr)
      cur_level_tabs|= tbl->table->map;
    else if (dep_tabs.push_back(tbl, sl_info->sl->join->thd->mem_root))
      return true;
  }

  /* Update current JOIN level information */
  sl_info->cur_level_tabs= cur_level_tabs;
  sl_info->forbid_extraction= false;
  sl_info->eq_info->empty();

  if (!(on_expr->used_tables() & (~cur_level_tabs)) &&
      (tabs_list->head()->outer_join &
       (JOIN_TYPE_LEFT|JOIN_TYPE_RIGHT)))
    sl_info->forbid_extraction= true;

  /* Try to extract new functionally dependent fields */
  if (get_on_expr_equalities_info(sl_info, on_expr))
    return true;
  get_new_dependencies_from_eq_info(sl_info);

  for (int i= dep_tabs.elements - 1; i >= 0; i--)
  {
    TABLE_LIST *tbl= dep_tabs.elem(i);
    if (!tbl->on_expr)
      continue;
    if (tbl->table)
    {
      sl_info->eq_info->empty();
      sl_info->cur_level_tabs= tbl->table->map;
      sl_info->forbid_extraction= false;

      if (!(tbl->on_expr->used_tables() & (~sl_info->cur_level_tabs)))
        sl_info->forbid_extraction= true;
      if (get_on_expr_equalities_info(sl_info, tbl->on_expr))
        return true;
      get_new_dependencies_from_eq_info(sl_info);
    }
    else if (extract_table_list_func_dep_fields(sl_info,
                                                &tbl->nested_join->join_list,
                                                tbl->on_expr))
      return true;
  }
  return false;
}


/**
  @brief
    Extract functionally dependent fields for the most outer JOIN tables

  @param
    sl_info  information about the current JOIN level and SELECT

  @details
    Consider the most outer JOIN.
    Collect this JOIN level tables and try to extract new functionally
    dependent fields of these tables using WHERE clause equalities and
    GROUP BY fields.
    If there is a table in the weak part of this JOIN try to extract its fields
    using ON expression of the considered JOIN.
    Otherwise, if the weak part of this JOIN contains nested join call
    extract_table_list_func_dep_fields() for this nested join.

  @retval
    true   if an error occurs
    false  otherwise
*/

bool extract_top_table_list_func_dep_fields(FD_select_info *sl_info)
{
  List<TABLE_LIST> dep_tabs;
  table_map cur_level_tabs= 0;
  List_iterator_fast<TABLE_LIST> it(*sl_info->sl->join->join_list);
  TABLE_LIST *tbl;

  while ((tbl= it++))
  {
    if (tbl->jtbm_subselect)
      continue;
    if (!tbl->on_expr && tbl->table)
      cur_level_tabs|= tbl->table->map;
    else if (dep_tabs.push_back(tbl, sl_info->sl->join->thd->mem_root))
      return true;
  }

  sl_info->cur_level_tabs= cur_level_tabs;
  sl_info->eq_info->empty();
  if (check_where_and_get_equalities_info(sl_info))
    return true;
  get_new_dependencies_from_eq_info(sl_info);

  for (int i= dep_tabs.elements - 1; i >= 0; i--)
  {
    TABLE_LIST *tbl= dep_tabs.elem(i);
    if (!tbl->on_expr)
      continue;
    if (tbl->table)
    {
      sl_info->eq_info->empty();
      sl_info->cur_level_tabs= tbl->table->map;
      if (get_on_expr_equalities_info(sl_info, tbl->on_expr))
        return true;
      get_new_dependencies_from_eq_info(sl_info);
    }
    else if (extract_table_list_func_dep_fields(sl_info,
                                                &tbl->nested_join->join_list,
                                                tbl->on_expr))
      return true;
  }
  return false;
}

/**
  If UPDATE query is used mark all fields of the updated table as 'allowed'.
*/

void set_update_table_fields(st_select_lex *sl)
{
  if (!sl->master_unit()->item ||
      !sl->master_unit()->outer_select() ||
      sl->master_unit()->outer_select()->join)
    return;
  List_iterator<TABLE_LIST> it(sl->master_unit()->outer_select()->leaf_tables);
  TABLE_LIST *tbl;
  while ((tbl= it++))
    bitmap_set_all(&tbl->table->tmp_set);
}



/**
  Set subqueries places in the SELECT sl.
  Place here: where this subquery is used (in SELECT list, WHERE or
  HAVING clause of sl).
*/

static
void set_subqueries_context(st_select_lex *sl)
{
  List_iterator_fast<Item> it(sl->item_list);
  Item *item;

  enum_parsing_place ctx= SELECT_LIST;
  while ((item= it++))
  {
    if (item->with_subquery())
      item->walk(&Item::set_subquery_ctx_processor, 0, &ctx);
  }

  Item *cond= sl->join->conds;
  if (cond && cond->with_subquery())
  {
    ctx= IN_WHERE;
    cond->walk(&Item::set_subquery_ctx_processor, 0, &ctx);
  }

  Item *having= sl->join->having;
  if (having && having->with_subquery())
  {
    ctx= IN_HAVING;
    having->walk(&Item::set_subquery_ctx_processor, 0, &ctx);
  }
}


/**
  Check if SELECT list items consists of constants and/or
  'allowed' fields only.
*/

bool are_select_list_fields_allowed(st_select_lex *sl,
                                    List<Item> *gb_items)
{
  List_iterator<Item> li(sl->item_list);
  Item *item;
  Item *err_item= 0;
  while ((item=li++))
  {
    if (item->excl_func_dep_on_grouping_fields(sl, gb_items,
                                               false, &err_item))
      continue;
    my_error(ER_NON_GROUPING_FIELD_USED, MYF(0),
             err_item->real_item()->full_name(), "SELECT list");
    return false;
  }
  return true;
}


/**
  Check if HAVING items consists of constants and/or
  'allowed' fields only.
*/

static
bool are_having_fields_allowed(st_select_lex *sl,
                               Item *having,
                               List<Item> *gb_items)
{
  if (!having || !sl->master_unit()->outer_select())
    return true;

  Item *err_item= 0;
  if (having->excl_func_dep_on_grouping_fields(sl, gb_items,
                                               false, &err_item))
    return true;
  my_error(ER_NON_GROUPING_FIELD_USED, MYF(0),
           err_item->real_item()->full_name(), "HAVING clause");
  return false;
}


/**
  Check if ORDER BY items consists of constants and/or
  'allowed' fields only.
*/

static
bool are_order_by_fields_allowed(st_select_lex *sl,
                                 List<Item> *gb_items)
{
  if (sl->order_list.elements == 0)
    return true;

  Item *err_item= 0;
  for (ORDER *order= sl->order_list.first; order; order=order->next)
  {
    Item *ord_item= *order->item;
    if (ord_item->excl_func_dep_on_grouping_fields(sl, gb_items,
                                                   false, &err_item))
      continue;
    my_error(ER_NON_GROUPING_FIELD_USED, MYF(0),
             err_item->real_item()->full_name(), "ORDER BY clause");
    return false;
  }
  return true;
}


/**
  Check if this SELECT fields list, HAVING clause and ORDER BY
  items contains constants or 'allowed' fields only.
*/

bool are_select_fields_allowed(st_select_lex *sl, List<Item> *gb_items)
{
  if (!are_select_list_fields_allowed(sl, gb_items) ||
      !are_having_fields_allowed(sl, sl->join->having, gb_items) ||
      !are_order_by_fields_allowed(sl, gb_items))
    return false;
  return true;
}


/**
  @brief
    Check if this SELECT returns deterministic result.

  @details
    Check if SELECT list, HAVING clause and ORDER BY clause
    of this SELECT depend on 'allowed' fields and constants only.
    'Allowed' fields list is formed this way:
      a. GROUP BY fields.
      b. Fields that are functionally dependent on GROUP BY fields.

    Functionally dependent fields can be extracted from the WHERE
    clause equalities (the most outer JOIN tables) and ON expressions
    (nested JOIN tables or tables from the weak part of some JOIN).
    It is done recursively starting from the top level JOIN tables
    (WHERE condition) down through ON expressions of outer joins.

  @note
    If this SELECT is a subquery and it contains outer references
    on parent SELECTs tables, check that all of these references
    are also 'allowed'. Fields of SELECT list, HAVING clause,
    ORDER BY clause and WHERE clause are checked.

  @note
    This method is called after simplify_joins() call.

  @retval
    true   if an error occurs
    false  otherwise
*/

bool st_select_lex::check_func_dep()
{
  /* Stop if no tables are used or fake SELECT is processed. */
  if (leaf_tables.is_empty() ||
      select_number == UINT_MAX ||
      select_number == INT_MAX)
    return false;

  bool need_check= (group_list.elements > 0) ||
                    (master_unit()->outer_select() &&
                     master_unit()->outer_select()->join) ||
                     having;

  List_iterator<TABLE_LIST> it(leaf_tables);
  TABLE_LIST *tbl;

  while ((tbl= it++))
  {
    if (!tbl->table)
      continue;
    bitmap_clear_all(&tbl->table->tmp_set);
  }
  set_update_table_fields(this); /* UPDATE query processing. */
  set_subqueries_context(this); /* Set subqueries places in this SELECT. */

  if (group_list.elements == 0 && !having)
  {
    /*
      This SELECT has no GROUP BY clause and HAVING.
      If so all FROM clause tables fields are 'allowed'.
    */
    List_iterator<TABLE_LIST> it(leaf_tables);
    TABLE_LIST *tbl;

    while ((tbl= it++))
    {
      bitmap_set_all(&tbl->table->tmp_set);
    }
    if (!need_check)
      return false;
  }

  List<Item> gb_items;
  /* Collect fields from GROUP BY. */
  if (collect_gb_items(this, gb_items))
    return true;

  if (olap != UNSPECIFIED_OLAP_TYPE)
  {
    /* If ROLLUP is used don't extract functionally dependent fields. */
    if (!are_select_fields_allowed(this, &gb_items))
      return true;
    return false;
  }

  List<Item_equal_fd_info> eq_info;
  FD_select_info *sl_info=
    new (join->thd->mem_root) FD_select_info(this, &eq_info);
  if (extract_top_table_list_func_dep_fields(sl_info))
    return true;

  /*
    Check if SELECT list and HAVING clause depend on 'allowed' fields only.
  */
  if (!are_select_fields_allowed(this, &gb_items))
    return true;

  return false;
}
