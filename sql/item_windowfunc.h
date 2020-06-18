#ifndef ITEM_WINDOWFUNC_INCLUDED
#define ITEM_WINDOWFUNC_INCLUDED

#include "my_global.h"
#include "item.h"

class Window_spec;


int test_if_group_changed(List<Cached_item> &list);

/* A wrapper around test_if_group_changed */
class Group_bound_tracker
{
public:

  Group_bound_tracker(THD *thd, SQL_I_List<ORDER> *list)
  {
    for (ORDER *curr = list->first; curr; curr=curr->next) 
    {
      Cached_item *tmp= new_Cached_item(thd, curr->item[0], TRUE);
      group_fields.push_back(tmp);
    }
  }

  void init()
  {
    first_check= true;
  }

  /*
    Check if the current row is in a different group than the previous row
    this function was called for.
    XXX: Side-effect: The new row's group becomes the current row's group.

    Returns true if there is a change between the current_group and the cached
    value, or if it is the first check after a call to init.
  */
  bool check_if_next_group()
  {
    if (test_if_group_changed(group_fields) > -1 || first_check)
    {
      first_check= false;
      return true;
    }
    return false;
  }

  /*
    Check if the current row is in a different group than the previous row
    check_if_next_group was called for.

    Compares the groups without the additional side effect of updating the
    current cached values.
  */
  int compare_with_cache()
  {
    List_iterator<Cached_item> li(group_fields);
    Cached_item *ptr;
    int res;
    while ((ptr= li++))
    {
      if ((res= ptr->cmp_read_only()))
        return res;
    }
    return 0;
  }
  ~Group_bound_tracker()
  {
    group_fields.delete_elements();
  }

private:
  List<Cached_item> group_fields;
  /*
    During the first check_if_next_group, the list of cached_items is not
    initialized. The compare function will return that the items match if
    the field's value is the same as the Cached_item's default value (0).
    This flag makes sure that we always return true during the first check.

    XXX This is better to be implemented within test_if_group_changed, but
    since it is used in other parts of the codebase, we keep it here for now.
  */
   bool first_check;
};

/*
  ROW_NUMBER() OVER (...)

  @detail
  - This is a Window function (not just an aggregate)
  - It can be computed by doing one pass over select output, provided 
    the output is sorted according to the window definition.
*/

class Item_sum_row_number: public Item_sum_int
{
  longlong count;

public:

  Item_sum_row_number(THD *thd)
    : Item_sum_int(thd),  count(0) {}

  void clear()
  {
    count= 0;
  }

  bool add()
  {
    count++;
    return false;
  }

  void update_field() {}

  enum Sumfunctype sum_func() const
  {
    return ROW_NUMBER_FUNC;
  }

  longlong val_int()
  {
    return count;
  }
  const char*func_name() const
  {
    return "row_number";
  }

  Item *get_copy(THD *thd, MEM_ROOT *mem_root)
  { return get_item_copy<Item_sum_row_number>(thd, mem_root, this); }
};


/*
  RANK() OVER (...) Windowing function

  @detail
  - This is a Window function (not just an aggregate)
  - It can be computed by doing one pass over select output, provided 
    the output is sorted according to the window definition.

  The function is defined as:

  "The rank of row R is defined as 1 (one) plus the number of rows that 
  precede R and are not peers of R"

  "This implies that if two or more rows are not distinct with respect to 
  the window ordering, then there will be one or more"
*/

class Item_sum_rank: public Item_sum_int
{
protected:
  longlong row_number; // just ROW_NUMBER()
  longlong cur_rank;   // current value

  Group_bound_tracker *peer_tracker;
public:

  Item_sum_rank(THD *thd) : Item_sum_int(thd), peer_tracker(NULL) {}

  void clear()
  {
    /* This is called on partition start */
    cur_rank= 1;
    row_number= 0;
  }

  bool add();

  longlong val_int()
  {
    return cur_rank;
  }

  void update_field() {}
  /*
   void reset_field();
    TODO: ^^ what does this do ? It is not called ever?
  */

  enum Sumfunctype sum_func () const
  {
    return RANK_FUNC;
  }

  const char*func_name() const
  {
    return "rank";
  }

  void setup_window_func(THD *thd, Window_spec *window_spec);

  void cleanup()
  {
    if (peer_tracker)
    {
      delete peer_tracker;
      peer_tracker= NULL;
    }
    Item_sum_int::cleanup();
  }
  Item *get_copy(THD *thd, MEM_ROOT *mem_root)
  { return get_item_copy<Item_sum_rank>(thd, mem_root, this); }
};


/*
  DENSE_RANK() OVER (...) Windowing function

  @detail
  - This is a Window function (not just an aggregate)
  - It can be computed by doing one pass over select output, provided 
    the output is sorted according to the window definition.

  The function is defined as:

  "If DENSE_RANK is specified, then the rank of row R is defined as the 
  number of rows preceding and including R that are distinct with respect 
  to the window ordering"

  "This implies that there are no gaps in the sequential rank numbering of
  rows in each window partition."
*/


class Item_sum_dense_rank: public Item_sum_int
{
  longlong dense_rank;
  bool first_add;
  Group_bound_tracker *peer_tracker;
 public:
  /*
     XXX(cvicentiu) This class could potentially be implemented in the rank
     class, with a switch for the DENSE case.
  */
  void clear()
  {
    dense_rank= 0;
    first_add= true;
  }
  bool add();
  void update_field() {}
  longlong val_int()
  {
    return dense_rank;
  }

  Item_sum_dense_rank(THD *thd)
    : Item_sum_int(thd), dense_rank(0), first_add(true), peer_tracker(NULL) {}
  enum Sumfunctype sum_func () const
  {
    return DENSE_RANK_FUNC;
  }

  const char*func_name() const
  {
    return "dense_rank";
  }

  void setup_window_func(THD *thd, Window_spec *window_spec);

  void cleanup()
  {
    if (peer_tracker)
    {
      delete peer_tracker;
      peer_tracker= NULL;
    }
    Item_sum_int::cleanup();
  }
  Item *get_copy(THD *thd, MEM_ROOT *mem_root)
  { return get_item_copy<Item_sum_dense_rank>(thd, mem_root, this); }
};

class Item_sum_hybrid_simple : public Item_sum,
                               public Type_handler_hybrid_field_type
{
 public:
  Item_sum_hybrid_simple(THD *thd, Item *arg):
   Item_sum(thd, arg),
   Type_handler_hybrid_field_type(MYSQL_TYPE_LONGLONG),
   value(NULL)
  { collation.set(&my_charset_bin); }

  Item_sum_hybrid_simple(THD *thd, Item *arg1, Item *arg2):
   Item_sum(thd, arg1, arg2),
   Type_handler_hybrid_field_type(MYSQL_TYPE_LONGLONG),
   value(NULL)
  { collation.set(&my_charset_bin); }

  bool add();
  bool fix_fields(THD *, Item **);
  void setup_hybrid(THD *thd, Item *item);
  double val_real();
  longlong val_int();
  my_decimal *val_decimal(my_decimal *);
  void reset_field();
  String *val_str(String *);
  /* TODO(cvicentiu) copied from Item_sum_hybrid, what does it do? */
  bool keep_field_type(void) const { return 1; }
  enum Item_result result_type() const
  { return Type_handler_hybrid_field_type::result_type(); }
  enum Item_result cmp_type() const
  { return Type_handler_hybrid_field_type::cmp_type(); }
  enum enum_field_types field_type() const
  { return Type_handler_hybrid_field_type::field_type(); }
  void update_field();
  Field *create_tmp_field(bool group, TABLE *table);
  void clear()
  {
    value->clear();
    null_value= 1;
  }

 private:
  Item_cache *value;
};

/*
   This item will remember the first value added to it. It will not update
   the value unless it is cleared.
*/
class Item_sum_first_value : public Item_sum_hybrid_simple
{
 public:
  Item_sum_first_value(THD* thd, Item* arg_expr) :
    Item_sum_hybrid_simple(thd, arg_expr) {}


  enum Sumfunctype sum_func () const
  {
    return FIRST_VALUE_FUNC;
  }

  const char*func_name() const
  {
    return "first_value";
  }

  Item *get_copy(THD *thd, MEM_ROOT *mem_root)
  { return get_item_copy<Item_sum_first_value>(thd, mem_root, this); }
};

/*
   This item will remember the last value added to it.

   This item does not support removal, and can be cleared only by calling
   clear().
*/
class Item_sum_last_value : public Item_sum_hybrid_simple
{
 public:
  Item_sum_last_value(THD* thd, Item* arg_expr) :
    Item_sum_hybrid_simple(thd, arg_expr) {}

  enum Sumfunctype sum_func() const
  {
    return LAST_VALUE_FUNC;
  }

  const char*func_name() const
  {
    return "last_value";
  }

  Item *get_copy(THD *thd, MEM_ROOT *mem_root)
  { return get_item_copy<Item_sum_last_value>(thd, mem_root, this); }
};

class Item_sum_nth_value : public Item_sum_hybrid_simple
{
 public:
  Item_sum_nth_value(THD *thd, Item *arg_expr, Item* offset_expr) :
    Item_sum_hybrid_simple(thd, arg_expr, offset_expr) {}

  enum Sumfunctype sum_func() const
  {
    return NTH_VALUE_FUNC;
  }

  const char*func_name() const
  {
    return "nth_value";
  }

  Item *get_copy(THD *thd, MEM_ROOT *mem_root)
  { return get_item_copy<Item_sum_nth_value>(thd, mem_root, this); }
};

class Item_sum_lead : public Item_sum_hybrid_simple
{
 public:
  Item_sum_lead(THD *thd, Item *arg_expr, Item* offset_expr) :
    Item_sum_hybrid_simple(thd, arg_expr, offset_expr) {}

  enum Sumfunctype sum_func() const
  {
    return LEAD_FUNC;
  }

  const char*func_name() const
  {
    return "lead";
  }

  Item *get_copy(THD *thd, MEM_ROOT *mem_root)
  { return get_item_copy<Item_sum_lead>(thd, mem_root, this); }
};

class Item_sum_lag : public Item_sum_hybrid_simple
{
 public:
  Item_sum_lag(THD *thd, Item *arg_expr, Item* offset_expr) :
    Item_sum_hybrid_simple(thd, arg_expr, offset_expr) {}

  enum Sumfunctype sum_func() const
  {
    return LAG_FUNC;
  }

  const char*func_name() const
  {
    return "lag";
  }

  Item *get_copy(THD *thd, MEM_ROOT *mem_root)
  { return get_item_copy<Item_sum_lag>(thd, mem_root, this); }
};

/*
  A base window function (aggregate) that also holds a counter for the number
  of rows.
*/
class Item_sum_window_with_row_count : public Item_sum_num
{
 public:
  Item_sum_window_with_row_count(THD *thd) : Item_sum_num(thd),
                                             partition_row_count_(0) {}

  Item_sum_window_with_row_count(THD *thd, Item *arg) :
    Item_sum_num(thd, arg), partition_row_count_(0) {};

  void set_row_count(ulonglong count) { partition_row_count_ = count; }

 protected:
  longlong get_row_count() { return partition_row_count_; }
 private:
  ulonglong partition_row_count_;
};

/*
  @detail
  "The relative rank of a row R is defined as (RK-1)/(NR-1), where RK is 
  defined to be the RANK of R and NR is defined to be the number of rows in
  the window partition of R."

  Computation of this function requires two passes:
  - First pass to find #rows in the partition
    This is held within the row_count context.
  - Second pass to compute rank of current row and the value of the function
*/
class Item_sum_percent_rank: public Item_sum_window_with_row_count
{
 public:
  Item_sum_percent_rank(THD *thd)
    : Item_sum_window_with_row_count(thd), cur_rank(1), peer_tracker(NULL) {}

  longlong val_int()
  {
   /*
      Percent rank is a real value so calling the integer value should never
      happen. It makes no sense as it gets truncated to either 0 or 1.
   */
    DBUG_ASSERT(0);
    return 0;
  }

  double val_real()
  {
   /*
     We can not get the real value without knowing the number of rows
     in the partition. Don't divide by 0.
   */
   ulonglong partition_rows = get_row_count();
   null_value= partition_rows > 0 ? false : true;

   return partition_rows > 1 ?
             static_cast<double>(cur_rank - 1) / (partition_rows - 1) : 0;
  }

  enum Sumfunctype sum_func () const
  {
    return PERCENT_RANK_FUNC;
  }

  const char*func_name() const
  {
    return "percent_rank";
  }

  void update_field() {}

  void clear()
  {
    cur_rank= 1;
    row_number= 0;
  }
  bool add();
  enum Item_result result_type () const { return REAL_RESULT; }
  enum_field_types field_type() const { return MYSQL_TYPE_DOUBLE; }

  bool fix_length_and_dec()
  {
    decimals = 10;  // TODO-cvicentiu find out how many decimals the standard
                    // requires.
    return FALSE;
  }

  void setup_window_func(THD *thd, Window_spec *window_spec);
  Item *get_copy(THD *thd, MEM_ROOT *mem_root)
  { return get_item_copy<Item_sum_percent_rank>(thd, mem_root, this); }

 private:
  longlong cur_rank;   // Current rank of the current row.
  longlong row_number; // Value if this were ROW_NUMBER() function.

  Group_bound_tracker *peer_tracker;

  void cleanup()
  {
    if (peer_tracker)
    {
      delete peer_tracker;
      peer_tracker= NULL;
    }
    Item_sum_num::cleanup();
  }
};




/*
  @detail
  "The relative rank of a row R is defined as NP/NR, where 
  - NP is defined to be the number of rows preceding or peer with R in the 
    window ordering of the window partition of R
  - NR is defined to be the number of rows in the window partition of R.

  Just like with Item_sum_percent_rank, computation of this function requires
  two passes.
*/

class Item_sum_cume_dist: public Item_sum_window_with_row_count
{
 public:
  Item_sum_cume_dist(THD *thd) : Item_sum_window_with_row_count(thd),
                                 current_row_count_(0) {}

  double val_real()
  {
    if (get_row_count() == 0)
    {
      null_value= true;
      return 0;
    }
    ulonglong partition_row_count= get_row_count();
    null_value= false;
    return static_cast<double>(current_row_count_) / partition_row_count;
  }

  bool add()
  {
    current_row_count_++;
    return false;
  }

  enum Sumfunctype sum_func() const
  {
    return CUME_DIST_FUNC;
  }

  void clear()
  {
    current_row_count_= 0;
    set_row_count(0);
  }

  const char*func_name() const
  {
    return "cume_dist";
  }

  void update_field() {}
  enum Item_result result_type () const { return REAL_RESULT; }
  enum_field_types field_type() const { return MYSQL_TYPE_DOUBLE; }

  bool fix_length_and_dec()
  {
    decimals = 10;  // TODO-cvicentiu find out how many decimals the standard
                    // requires.
    return FALSE;
  }
  
  Item *get_copy(THD *thd, MEM_ROOT *mem_root)
  { return get_item_copy<Item_sum_cume_dist>(thd, mem_root, this); }

 private:
  ulonglong current_row_count_;
};

class Item_sum_ntile : public Item_sum_window_with_row_count
{
 public:
  Item_sum_ntile(THD* thd, Item* num_quantiles_expr) :
    Item_sum_window_with_row_count(thd, num_quantiles_expr),
    current_row_count_(0),
    n_old_val_(0) {};

  double val_real()
  {
    return (double) val_int();
  }

  longlong val_int()
  {
    if (get_row_count() == 0)
    {
      null_value= true;
      return 0;
    }

    longlong num_quantiles= get_num_quantiles();

    if (num_quantiles <= 0 || 
      (static_cast<ulonglong>(num_quantiles) != n_old_val_ && n_old_val_ > 0))
    {
      my_error(ER_INVALID_NTILE_ARGUMENT, MYF(0));
      return true;
    }
    n_old_val_= static_cast<ulonglong>(num_quantiles);
    null_value= false;
    ulonglong quantile_size = get_row_count() / num_quantiles;
    ulonglong extra_rows = get_row_count() - quantile_size * num_quantiles;

    if (current_row_count_ <= extra_rows * (quantile_size + 1))
      return (current_row_count_ - 1) / (quantile_size + 1) + 1;

    return (current_row_count_ - 1 - extra_rows) / quantile_size + 1;
  }

  bool add()
  {
    current_row_count_++;
    return false;
  }

  enum Sumfunctype sum_func() const
  {
    return NTILE_FUNC;
  }

  void clear()
  {
    current_row_count_= 0;
    set_row_count(0);
    n_old_val_= 0;
  }

  const char*func_name() const
  {
    return "ntile";
  }

  void update_field() {}

  enum Item_result result_type () const { return INT_RESULT; }
  enum_field_types field_type() const { return MYSQL_TYPE_LONGLONG; }
  
  Item *get_copy(THD *thd, MEM_ROOT *mem_root)
  { return get_item_copy<Item_sum_ntile>(thd, mem_root, this); }

 private:
  longlong get_num_quantiles() { return args[0]->val_int(); }
  ulong current_row_count_;
  ulonglong n_old_val_;
};


class Item_window_func : public Item_func_or_sum
{
  /* Window function parameters as we've got them from the parser */
public:
  LEX_STRING *window_name;
public:
  Window_spec *window_spec;
  
public:
  Item_window_func(THD *thd, Item_sum *win_func, LEX_STRING *win_name)
    : Item_func_or_sum(thd, (Item *) win_func),
      window_name(win_name), window_spec(NULL), 
      force_return_blank(true),
      read_value_from_result_field(false) {}

  Item_window_func(THD *thd, Item_sum *win_func, Window_spec *win_spec)
    : Item_func_or_sum(thd, (Item *) win_func), 
      window_name(NULL), window_spec(win_spec), 
      force_return_blank(true),
      read_value_from_result_field(false) {}

  Item_sum *window_func() const { return (Item_sum *) args[0]; }

  void update_used_tables();

  /*
    This is used by filesort to mark the columns it needs to read (because they
    participate in the sort criteria and/or row retrieval. Window functions can
    only be used in sort criteria).

    Sorting by window function value is only done after the window functions
    have been computed. In that case, window function will need to read its
    temp.table field. In order to allow that, mark that field in the read_set.
  */
  bool register_field_in_read_map(void *arg)
  {
    TABLE *table= (TABLE*) arg;
    if (result_field && (result_field->table == table || !table))
    {
      bitmap_set_bit(result_field->table->read_set, result_field->field_index);
    }
    return 0;
  }

  bool is_frame_prohibited() const
  {
    switch (window_func()->sum_func()) {
    case Item_sum::ROW_NUMBER_FUNC:
    case Item_sum::RANK_FUNC:
    case Item_sum::DENSE_RANK_FUNC:
    case Item_sum::PERCENT_RANK_FUNC:
    case Item_sum::CUME_DIST_FUNC:
    case Item_sum::NTILE_FUNC:
      return true;
    default: 
      return false;
    }
  }

  bool requires_special_cursors() const
  {
    switch (window_func()->sum_func()) {
    case Item_sum::FIRST_VALUE_FUNC:
    case Item_sum::LAST_VALUE_FUNC:
    case Item_sum::NTH_VALUE_FUNC:
    case Item_sum::LAG_FUNC:
    case Item_sum::LEAD_FUNC:
      return true;
    default:
      return false;
    }
  }

  bool requires_partition_size() const
  {
    switch (window_func()->sum_func()) {
    case Item_sum::PERCENT_RANK_FUNC:
    case Item_sum::CUME_DIST_FUNC:
    case Item_sum::NTILE_FUNC:
      return true;
    default:
      return false;
    }
  }

  bool requires_peer_size() const
  {
    switch (window_func()->sum_func()) {
    case Item_sum::CUME_DIST_FUNC:
      return true;
    default:
      return false;
    }
  }

  bool is_order_list_mandatory() const
  {
    switch (window_func()->sum_func()) {
    case Item_sum::RANK_FUNC:
    case Item_sum::DENSE_RANK_FUNC:
    case Item_sum::PERCENT_RANK_FUNC:
    case Item_sum::CUME_DIST_FUNC:
    case Item_sum::LAG_FUNC:
    case Item_sum::LEAD_FUNC:
      return true;
    default: 
      return false;
    }
  }  

  /*
    Computation functions.
    TODO: consoder merging these with class Group_bound_tracker.
  */
  void setup_partition_border_check(THD *thd);

  enum_field_types field_type() const
  { 
    return ((Item_sum *) args[0])->field_type(); 
  }
  enum Item::Type type() const { return Item::WINDOW_FUNC_ITEM; }

private:
  /* 
    Window functions are very special functions, so val_() methods have
    special meaning for them:

    - Phase#1, "Initial" we run the join and put its result into temporary 
      table. For window functions, we write the default value (NULL?) as 
      a placeholder.
      
    - Phase#2: "Computation": executor does the scan in {PARTITION, ORDER BY} 
      order of this window function. It calls appropriate methods to inform 
      the window function about rows entering/leaving the window. 
      It calls window_func()->val_int() so that current window function value
      can be saved and stored in the temp.table.

    - Phase#3: "Retrieval" the temporary table is read and passed to query 
      output. However, Item_window_func still remains in the select list,
      so item_windowfunc->val_int() will be called.
      During Phase#3, read_value_from_result_field= true.
  */
  bool force_return_blank;
  bool read_value_from_result_field;

public:
  void set_phase_to_initial()
  {
    force_return_blank= true;
    read_value_from_result_field= false;
  }
  void set_phase_to_computation()
  {
    force_return_blank= false;
    read_value_from_result_field= false;
  }
  void set_phase_to_retrieval()
  {
    force_return_blank= false;
    read_value_from_result_field= true;
  }

  bool is_null()
  {
    if (force_return_blank)
      return true;

    if (read_value_from_result_field)
      return result_field->is_null();

    return window_func()->is_null();
  }

  double val_real() 
  {
    double res;
    if (force_return_blank)
    {
      res= 0.0;
      null_value= true;
    }
    else if (read_value_from_result_field)
    {
      res= result_field->val_real();
      null_value= result_field->is_null();
    }
    else
    {
      res= window_func()->val_real();
      null_value= window_func()->null_value;
    }
    return res;
  }

  longlong val_int()
  {
    longlong res;
    if (force_return_blank)
    {
      res= 0;
      null_value= true;
    }
    else if (read_value_from_result_field)
    {
      res= result_field->val_int();
      null_value= result_field->is_null();
    }
    else
    {
      res= window_func()->val_int();
      null_value= window_func()->null_value;
    }
    return res;
  }

  String* val_str(String* str)
  {
    String *res;
    if (force_return_blank)
    {
      null_value= true;
      res= NULL;
    }
    else if (read_value_from_result_field)
    {
      if ((null_value= result_field->is_null()))
        res= NULL;
      else
        res= result_field->val_str(str);
    }
    else
    {
      res= window_func()->val_str(str);
      null_value= window_func()->null_value;
    }
    return res;
  }

  my_decimal* val_decimal(my_decimal* dec)
  {
    my_decimal *res;
    if (force_return_blank)
    {
      null_value= true;
      res= NULL;
    }
    else if (read_value_from_result_field)
    {
      if ((null_value= result_field->is_null()))
        res= NULL;
      else
        res= result_field->val_decimal(dec);
    }
    else
    {
      res= window_func()->val_decimal(dec);
      null_value= window_func()->null_value;
    }
    return res;
  }

  void split_sum_func(THD *thd, Ref_ptr_array ref_pointer_array,
                              List<Item> &fields, uint flags);

  bool fix_length_and_dec()
  {
    decimals = window_func()->decimals;
    unsigned_flag= window_func()->unsigned_flag;
    return FALSE;
  }

  const char* func_name() const { return "WF"; }

  bool fix_fields(THD *thd, Item **ref);

  bool resolve_window_name(THD *thd);
  
  void print(String *str, enum_query_type query_type);

 Item *get_copy(THD *thd, MEM_ROOT *mem_root) { return 0; }

};

#endif /* ITEM_WINDOWFUNC_INCLUDED */
