#include "mariadb.h"
#include "sql_base.h"
#include "sql_select.h"

/**
  @file
    Create a set of fields that are functionally dependent on some
    initial set.

    Consider some table T and subsets of its columns: A and B.
    A and B can be empty and can intersect.
    B is called functionally dependent on A of the same table T
    if such a rule holds: if any two rows from every column in A
    are compared as equal or both NULL then they are compared as
    equal or both NULL in B.

    I. Usage of functional dependencies in query parsing when GROUP BY is used.

    Rule 1:
    SQL Standard states that if GROUP BY is used the only fields that can
    be used in SELECT list, HAVING and ORDER BY clauses are fields that
    are used in GROUP BY and fields that are functionally dependent on
    fields used in GROUP BY.

    So, the aim is to create a set of such functionally dependent fields (FD).
    The initial set (IS) is a set of GROUP BY fields there.

    Functionally dependent fields can be derived from:

    1. PRIMARY and/or UNIQUE keys

       If initial set fields determines some PRIMARY or UNIQUE key all of
       their table fields are added to FD.
       Note: if UNIQUE key is used it shouldn't contain NULL columns.

    2. From Virtual column definition.

       If some virtual column is defined by IS fields then this virtual column
       is added to FD.

    3. Materialized derived tables/views

       Materialized derived tables and views are checked before the query where
       they are used. So if a UNION that defines materialized derived table
       or view contains a SELECT for which 'Rule 1' doesn't apply the SELECT
       where this derived table or view is used will never be entered.
       So, it can be said that materialized derived table or view SELECT list
       uses IS or FD columns only.
       Therefore, if the materialized derived table or view field is in IS of
       the SELECT where it is used then all of this derived table or view
       fields can be added to FD.
       Note: doesn't apply to the inner LEFT JOIN tables.

    4. Equality predicates

       FD fields can be extracted from the equality predicates of the form:

       F2 = g(F1,...,Fn), (1)

       where F2          - FD candidate,
             (F1,...,Fn) - IS fields and constants,
             g()         - some deterministic function.

       If no conversion is applied to F2 then F2 is FD field.
       (F1,...Fn) can include functionally dependent fields also.

       Deterministic function is a function that returns the same result set
       every time it is called with the same input set. Function can be called
       non-deterministic if at least one of its function arguments is
       non-deterministic.
       Deterministic expression is an expression that uses only deterministic
       functions.

       E.g.
         Non-deterministic expression:
         1 + rand(),

         where rand() - non-deterministic function.

      4.1.  WHERE clause equality predicates.

      FD fields can be extracted from the top AND level WHERE clause
      equality predicates of the form (1).

      4.2.  ON expressions equality predicates

      FD fields can be extracted from the top AND level ON expression clauses
      equality predicates of the form (1).

      FD fields set is created after all possible JOINs are simplified in
      simplify_joins(). On this step ON expressions are used with LEFT JOINs
      only and contains fields from the left and right LEFT join tables only
      (including outer SELECT fields).

      When ON expression is checked following conditions should be met:
      1. LEFT JOIN left table fields can't be extracted.
         So F2 in (1) is always some inner table field of the considered
         LEFT JOIN.
      2. If in (1) more than one field is used in the right part of the
         equality all of these fields should be inner tables fields.
      3. If at least one top AND level conjunct is non-deterministic than
         it is forbidden to new functionally dependent fields at the considered
         JOIN level.
         E.g.
         rand() function

      4. If at least one top AND level conjunct contains non GROUP BY field
         from the left LEFT JOIN table then it is forbidden to extract new
         functionally dependent fields at the considered JOIN level.
      5. If there are no conjuncts that depend on this level LEFT JOIN left
         table then it is forbidden to extract new functionally dependent
         fields at the considered JOIN level.
         Note: doesn't apply to the most outer ON expression.

    II. Functionally dependent fields extraction algorithm for I.

    a. All fields used in GROUP BY are marked.

    b. Recursively starting from the top level (WHERE condition) down through
       ON expressions (starting from the most outer LEFT JOIN ON expression
       down to the most inner LEFT JOIN ON expressions):

    1.1 Go through the top AND level equality predicates and check if they meet
        the conditions above. Take appropriate actions if conditions are
        not met for the equality predicate, otherwise save as usable.

        Note: Saved equality predicate can not depend on GROUP BY fields
              or constants. New functionally dependent fields can be extracted
              from some other this level equality predicates and they can make
              the considered non-dependent equality predicate usable.

        Special case of (3):
          F2 = I(F1) (3')

        Conversion is not applied to both F1 and F2 (F1 and F2 are of the
        same type as the equality), F1 is neither 'gb' or 'fd' field or
        constant.
        Then (3') equality can be considered as two equalities:

        F2 = I(F1) and F1 = I(F2)

    1* If ON expression is considered all of its top AND level conjuncts are
       checked if all restrictions are performed.
    2. Expand the set of functionally dependent fields with saved equality
       predicates until it is possible.
    2.1 Go through usable equality predicates and try to extract new
        functionally dependent fields. Delete from this list no longer
        necessary equality predicates.
    2.2 If any functionally dependent fields were extracted and usable
        equality predicates list is not empty repeat 2.1.

    By the end of this algorithm all fields that are used in GROUP BY
    and fields functionally dependent on them are marked. These fields
    are allowed to be used in SELECT list, HAVING and ORDER BY clauses.

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
  /*
    Set of fields (Item_field and Item_ref items) from the other part
    of the equality. Needs to be IS or FD fields.
  */
  List<Item> dp_items;
  Item_equal_fd_info(Field *nd_f, List<Item> dp_i) :
    nd_field(nd_f), dp_items(dp_i) {}
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
  bool top_level; // TRUE if the most outer LEFT JOIN is considered.
  /*
    If the current JOIN level is the most outer JOIN store WHERE clause
    equality predicates information that can be used in extraction of new
    functionally dependent fields.
    If the current JOIN is nested JOIN store its ON expression
    equality predicates information.
  */
  List<Item_equal_fd_info> *eq_info;
  table_map cur_level_tabs; // Map of current JOIN level tables.
  FD_select_info(st_select_lex *sel, List<Item_equal_fd_info> *eq_inf)
    : sl(sel), top_level(true),
      eq_info(eq_inf), cur_level_tabs(0) {}
};


/**
  Check if all columns that define a key are IS or FD fields.
  If so return true.
  If some index column takes NULL values this index can't be used.
*/

static bool are_all_key_fields_allowed(KEY *key)
{
  Item *err_item= 0;
  for (uint i= 0; i < key->user_defined_key_parts; i++)
  {
    /* Check if column can take NULL values. */
    if (key->key_part[i].field->null_ptr)
      return false;
    if (!key->key_part[i].field->
         excl_func_dep_on_grouping_fields(0, false, &err_item))
      return false;
  }
  return true;
}


/**
  @brief
    Check if PRIMARY or UNIQUE keys can expand FD fields list.

  @param
    sl  current select

  @details
    For each table used in the FROM list of SELECT sl check
    its PRIMARY and UNIQUE keys.
    If some table key consists of IS or FD fields only then
    all fields of this table are FD fields.

  @retval
    true   if FD fields set is expanded
    false  otherwise
*/

static
bool find_allowed_unique_keys(st_select_lex *sl)
{
  List_iterator<TABLE_LIST> it(sl->leaf_tables);
  TABLE_LIST *tbl;
  bool expanded= false;
  while ((tbl= it++))
  {
    if (!tbl->table)
      continue;
    /* Check if all fields of this table are already marked as FD. */
    if (bitmap_is_set_all(&tbl->table->tmp_set))
      continue;
    /* Check if PRIMARY key can expand FD fields list. */
    if (tbl->table->s->primary_key < MAX_KEY)
    {
      KEY *pk= &tbl->table->key_info[tbl->table->s->primary_key];
      if (are_all_key_fields_allowed(pk))
      {
        bitmap_set_all(&tbl->table->tmp_set);
        expanded= true;
        continue;
      }
    }
    /* Check if UNIQUE keys can expand FD fields list. */
    KEY *end= tbl->table->key_info + tbl->table->s->keys;
    for (KEY *k= tbl->table->key_info; k < end; k++)
      if ((k->flags & HA_NOSAME) && are_all_key_fields_allowed(k))
      {
        bitmap_set_all(&tbl->table->tmp_set);
        expanded= true;
        break;
      }
  }
  return expanded;
}


/**
  @brief
    Expand FD list with materialized derived table or view fields if possible.

  @param
    tbl  TABLE_LIST to check

  @details
    First check that tbl can be used for expansion of FD fields list.
    tbl should be a materialized derived table or view that is defined with a
    single SELECT and GROUP BY. If so that means that all fields of this
    materialized derived table or view are uniquely identified (materialized
    derived tables are checked before they are used).
    If some field of such tbl is functionally dependent on IS fields or
    is IS field (in SELECT where this tbl is used) then all fields
    of this materialized derived table or view are marked as FD as they
    are uniquely identified in this SELECT.
*/

inline void expand_fd_fields_with_mat_der(TABLE_LIST *tbl)
{
  if (tbl->is_materialized_derived() &&
      tbl->derived_uniq_ident)
  bitmap_set_all(&tbl->table->tmp_set);
}


/**
  @brief
    Collect fields used in GROUP BY.

  @param
    sl        current select
    gb_items  list of GROUP BY non-field items

  @details
    For each table used in the FROM clause of the SELECT sl collect
    its fields used in the GROUP BY of sl.
    Mark them in tmp_set map.
    If GROUP BY item is not a field store it in gb_items list.
    The created set is an IS.

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
      bitmap_set_bit(&fld->table->tmp_set,
                       fld->field_index);
    }
    else if (gb_items.push_back(ord_item, thd->mem_root))
      return true;
  }

  /* Check if IS fields are key fields and can expand FD fields set. */
  if (sl->olap == UNSPECIFIED_OLAP_TYPE)
    find_allowed_unique_keys(sl);
  return false;
}


/**
  @brief
    Get equality information so it can be used in extraction of FD field

  @param
    sl_info            information about the current JOIN level
                       and SELECT
    eq                 the considered equality
    curr_dep_part_idx  index of the equality part which is a field of some
                       current JOIN level table

  @details
    Check equality if it can be used in functionally dependent field extraction
    and if so collect its internal information.
    Equality should satisfy the restrictions from @file description.

  @note
    Also check if the equality use forbidden outer SELECTs fields.

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
  List<Item> curr_dep_fld;
  List<Item> op_part_flds;

  bool dep_curr=
    curr_dep_part->check_usage_in_fd_field_extraction(thd,
                                                      &curr_dep_fld,
                                                      &err_item);
  if (!dep_curr && err_item)
  {
    /*
      If the equality use forbidden outer SELECT field through error.
    */
    my_error(ER_NON_GROUPING_FIELD_USED, MYF(0),
             err_item->real_item()->full_name(), "WHERE clause");
    return true;
  }
  bool dep_op=
    op_equal_part->check_usage_in_fd_field_extraction(thd,
                                                      &op_part_flds,
                                                      &err_item);
  if ((!dep_op && err_item) ||                            // (1)
      (dep_curr && dep_op) ||                             // (2)
      (!dep_op && op_part_flds.is_empty()) ||             // (3)
      ((op_equal_part->type() == Item::FUNC_ITEM ||       // (4)
       op_equal_part->type() == Item::COND_ITEM) &&
       !((Item_func *)op_equal_part)->is_deterministic))
  {
    /*
      (1) Equality use a forbidden outer SELECT field.
      (2) Equality depend on 'allowed' fields only.
      (3) op_eq_part contains expression that can't forbids FD
          field extraction.
      (4) op_eq_part is non-deterministic expression.

      In these cases the equality can't be used in FD field extraction.
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
        (op_equal_part->real_item()->type() == Item::FIELD_ITEM) &&
        (op_equal_part->used_tables() & sl_info->cur_level_tabs) &&
        (eq->compare_type_handler() ==
           op_equal_part->type_handler_for_comparison()))
    {
      /*
        The other part of the equality can also be extracted as FD field.
      */
      Item_equal_fd_info *new_eq=
        new (thd->mem_root) Item_equal_fd_info(
              ((Item_field *)op_equal_part->real_item())->field,
               curr_dep_fld);
      if (sl_info->eq_info->push_back(new_eq, thd->mem_root))
        return true;
    }
  }
  else if (!dep_on_outer &&
           (op_equal_part->real_item()->type() == Item::FIELD_ITEM) &&
           (op_equal_part->used_tables() & sl_info->cur_level_tabs) &&
           (eq->compare_type_handler() ==
              op_equal_part->type_handler_for_comparison()))
  {
    /*
      The other part of the equality can also be extracted as FD field.
    */
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
    Check if the equality can be used in FD field extraction

  @param
    sl_info  information about the current JOIN level and SELECT
    eq       the considered equality
    checked  IN/OUT TRUE if eq is checked on forbidden outer SELECT
             fields in get_eq_info_for_fd_field_extraction() method.

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
  Check if item contains some forbidden outer SELECT field.
*/

static
bool check_on_forbidden_outer_fields(st_select_lex *sl,
                                     Item *item,
                                     const char *where)
{
  if (!sl->master_unit()->outer_select())
    return false;

  Item *err_item= 0;
  if (!item->excl_func_dep_on_grouping_fields(0, true, &err_item) &&
      err_item)
  {
    my_error(ER_NON_GROUPING_FIELD_USED, MYF(0),
             err_item->real_item()->full_name(), where);
    return true;
  }
  return false;
}


/**
  @brief
    Check if there are equality predicates usable in FD fields extraction.

  @param
    sl_info  information about the current JOIN level
             and SELECT
    expr     the considered expression
    where    the context of the expression

  @details
    Check upper AND level expr equality predicates if they can
    be used in extraction of a new functionally dependent field of the
    current JOIN level table.
    For this purpose call check_equality_usage_in_fd_field_extraction().
    If needed check top AND level expr conjuncts on usage of forbidden
    outer SELECT fields.

  @retval
    true   if an error occurs
    false  otherwise
*/

bool check_expr_and_get_equalities_info(FD_select_info *sl_info,
                                        Item *expr,
                                        const char *where)
{
  if (!expr)
    return false;
  bool checked= false;

  if ((expr->type() == Item::COND_ITEM) &&
      ((Item_cond*) expr)->functype() == Item_func::COND_AND_FUNC)
  {
    List_iterator_fast<Item> li(*((Item_cond*) expr)->argument_list());
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
               check_on_forbidden_outer_fields(sl_info->sl,
                                               item, where))
        return true;
    }
  }
  else if (expr->type() == Item::FUNC_ITEM &&
           ((Item_func*) expr)->functype() == Item_func::EQ_FUNC &&
           check_equality_usage_in_fd_field_extraction(sl_info,
                                                       (Item_func_eq *)expr,
                                                       &checked))
    return true;
  if (!checked &&
      check_on_forbidden_outer_fields(sl_info->sl, expr, where))
    return true;
  return false;
}


/**
  @brief
    Check if the considered ON expression can be used in FD fields extraction.

  @param
    sl_info  information about the current JOIN level
             and SELECT
    expr     the considered ON expression

  @details
    Check if ON expression:
    1. Contains non-deterministic functions.
       E.g. rand()
    2. Is not the most outer ON expression and doesn't contain
       LEFT JOIN left tables fields.
    3. Contains LEFT JOIN left tables fields that are not used in
       GROUP BY.
    In these cases from this ON expression no new FD fields can be
    extracted.

  @retval
    true   if an error occurs
    false  otherwise
*/

bool check_on_expr_and_get_equalities_info(FD_select_info *sl_info,
                                           Item *on_expr)
{
  if (!on_expr || on_expr->const_item())
    return false;

  if (((on_expr->type() == Item::COND_ITEM ||                              // 1
        on_expr->type() == Item::FUNC_ITEM) &&
        ((Item_func *)on_expr)->has_rand_bit()) ||
        (!sl_info->top_level &&                                            // 2
         !(on_expr->used_tables() & (~sl_info->cur_level_tabs))) ||
          ((on_expr->used_tables() & (~sl_info->cur_level_tabs)) &&        // 3
            on_expr->walk(&Item::check_reject_fd_extraction_processor, 0,
                        &sl_info->cur_level_tabs)))
  {
    /* Check if this ON expression doesn't contain FD fields. */
    if (check_on_forbidden_outer_fields(sl_info->sl, on_expr, "ON expression"))
      return true;
    return false;
  }

  if (check_expr_and_get_equalities_info(sl_info, on_expr, "ON expression"))
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
*/

static
void get_new_dependencies_from_eq_info(FD_select_info *sl_info)
{
  List<Item_equal_fd_info> *eq_info= sl_info->eq_info;
  /* Nothing to extract from. */
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
        /* Field is already found as 'allowed'. */
        li.remove();
        continue;
      }
      List_iterator_fast<Item> it(info->dp_items);
      Item *item;
      bool dep= true;

      while ((item= it++))
        dep&= item->excl_func_dep_on_grouping_fields(0, false, &err_item);
      if (!dep)
        continue;

      Field *fld= info->nd_field;
      /* Mark nd_field as FD field. */
      bitmap_set_bit(&fld->table->tmp_set, fld->field_index);
      /*
        Check if nd_field is in materialized derived table or view
        and all fields of this table becomes FD.
      */
      expand_fd_fields_with_mat_der(fld->table->pos_in_table_list);
      extracted= true;
      li.remove();
    }
    if (!extracted || eq_info->is_empty())
    {
      /* Check if any nd_field table key becomes usable for FD expansion. */
      if (find_allowed_unique_keys(sl_info->sl))
        extracted= true;
    }
  }
}


/**
  @brief
    Recursively extract FD fields of nested JOINs tables.

  @param
    sl_info    information about the current JOIN level and SELECT
    nest_tab   the considered nested join

  @details
    Collect this JOIN level (nest_tab) tables and try to expand FD fields
    sets of these tables using nest_tab ON expression (ON expression of
    the parent JOIN) equality predicates and IS fields.

    If there is a table on the right part of this LEFT JOIN try to extract
    its fields using ON expression of the considered LEFT JOIN.
    Otherwise, if the right part of this JOIN contains nested join recursively
    call this method for this nested join.

  @note
    Information about the considered JOIN level is stored in sl_info
    fields.

  @retval
    true   if an error occurs
    false  otherwise
*/

bool expand_table_list_fd_set(FD_select_info *sl_info, TABLE_LIST *nest_tab)
{
  List<TABLE_LIST> dep_tabs;
  table_map cur_level_tabs= 0;
  List_iterator_fast<TABLE_LIST> it(nest_tab->nested_join->join_list);
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
  sl_info->eq_info->empty();

  /* Try to extract new functionally dependent fields */
  if (check_on_expr_and_get_equalities_info(sl_info, nest_tab->on_expr))
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

      List<TABLE_LIST> left_tab;
      if (left_tab.push_back(tbl, sl_info->sl->join->thd->mem_root))
        return true;
      if (check_on_expr_and_get_equalities_info(sl_info, tbl->on_expr))
        return true;
      get_new_dependencies_from_eq_info(sl_info);
    }
    else if (expand_table_list_fd_set(sl_info, tbl))
      return true;
  }
  return false;
}


/**
  @brief
    Expand FD fields set of the most outer JOIN tables.

  @param
    sl_info  information about the current JOIN level and SELECT

  @details
    Consider the most outer JOIN.
    Collect this JOIN level tables and try to extract new functionally
    dependent fields of these tables using WHERE clause equality predicates
    and IS fields.
    If there is a table in the right part of this LEFT JOIN try to extract
    its fields using ON expression of the considered JOIN.
    Otherwise, if the right part of this JOIN contains nested join call
    expand_table_list_fd_set() for this nested join to extract
    FD fields.

  @retval
    true   if an error occurs
    false  otherwise
*/

bool expand_top_table_list_fd_set(FD_select_info *sl_info)
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
    {
      cur_level_tabs|= tbl->table->map;
      if (!bitmap_is_clear_all(&tbl->table->tmp_set))
        expand_fd_fields_with_mat_der(tbl);
    }
    else if (dep_tabs.push_back(tbl, sl_info->sl->join->thd->mem_root))
      return true;
  }

  sl_info->cur_level_tabs= cur_level_tabs;
  sl_info->eq_info->empty();
  if (check_expr_and_get_equalities_info(sl_info, sl_info->sl->join->conds,
                                         "WHERE clause"))
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
      List<TABLE_LIST> left_tab;
      if (left_tab.push_back(tbl, sl_info->sl->join->thd->mem_root))
        return true;
      if (check_on_expr_and_get_equalities_info(sl_info, tbl->on_expr))
        return true;
      get_new_dependencies_from_eq_info(sl_info);
    }
    else
    {
      if (tbl->outer_join & JOIN_TYPE_LEFT)
        sl_info->top_level= false;
      if (expand_table_list_fd_set(sl_info, tbl))
        return true;
    }
  }
  return false;
}

/**
  If UPDATE query is used mark all fields of the updated table as IS fields.
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
  Check if SELECT list items contain IS and FD fields only.
*/

bool are_select_list_fields_allowed(st_select_lex *sl,
                                    List<Item> *gb_items)
{
  List_iterator<Item> li(sl->item_list);
  Item *item;
  Item *err_item= 0;
  while ((item=li++))
  {
    if (item->excl_func_dep_on_grouping_fields(gb_items, false, &err_item))
      continue;
    my_error(ER_NON_GROUPING_FIELD_USED, MYF(0),
             err_item->real_item()->full_name(), "SELECT list");
    return false;
  }
  return true;
}


/**
  Check if HAVING clause contains IS and FD fields only.
*/

static
bool are_having_fields_allowed(st_select_lex *sl,
                               Item *having,
                               List<Item> *gb_items)
{
  if (!having || !sl->master_unit()->outer_select())
    return true;

  Item *err_item= 0;
  if (having->excl_func_dep_on_grouping_fields(gb_items, false, &err_item))
    return true;
  my_error(ER_NON_GROUPING_FIELD_USED, MYF(0),
           err_item->real_item()->full_name(), "HAVING clause");
  return false;
}


/**
  Check if ORDER BY items contain IS and FD fields only.
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
    if (ord_item->excl_func_dep_on_grouping_fields(gb_items, false, &err_item))
      continue;
    my_error(ER_NON_GROUPING_FIELD_USED, MYF(0),
             err_item->real_item()->full_name(), "ORDER BY clause");
    return false;
  }
  return true;
}


/**
  Check if this SELECT fields list, HAVING clause and ORDER BY
  items contains IS and FD fields only.
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
    Check if the SELECT list, HAVING clause and ORDER BY clause
    of this SELECT depend on IS and FD fields only.
    IS fields are this SELECT GROUP BY items.
    FD fields are fields that are functionally dependent on IS fields.

    Functionally dependent fields can be extracted from the WHERE
    clause equalities (the most outer JOIN tables) and ON expressions
    (nested JOIN tables or tables from the right part of some LEFT JOIN).
    It is done recursively starting from the top level LEFT JOIN tables
    (WHERE condition) down through ON expressions of outer joins.
    Also FD fields can be received from materialized derived tables or views,
    UNIQUE and PRIMARY keys, virtual columns definitions.

  @note
    If this SELECT is a subquery and it contains outer references
    on parent SELECTs tables, check that all of these references
    can be used and are IS or FD fields. Fields of SELECT list,
    HAVING clause, ORDER BY clause and WHERE clause are checked.

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

  if (group_list.elements == 0 && !having)
  {
    /*
      This SELECT has no GROUP BY clause and HAVING.
      If so all FROM clause tables fields are marked as IS fields.
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
    /* If ROLLUP is used don't expand FD fields set. */
    if (!are_select_fields_allowed(this, &gb_items))
      return true;
    return false;
  }

  List<Item_equal_fd_info> eq_info;
  FD_select_info *sl_info=
    new (join->thd->mem_root) FD_select_info(this, &eq_info);
  if (expand_top_table_list_fd_set(sl_info))
    return true;

  if (!are_select_fields_allowed(this, &gb_items))
    return true;

  return false;
}
