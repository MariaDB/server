#ifndef ITEM_WINDOWFUNC_INCLUDED
#define ITEM_WINDOWFUNC_INCLUDED

#include "my_global.h"
#include "item.h"

class Window_spec;

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

  Item_sum_row_number(THD *thd)
    : Item_sum_int(thd),  count(0) {}

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

  @todo: failure to overload val_int() causes infinite mutual recursion like
  this:

  #7505 0x0000555555cd3a1c in Item::val_int_from_real (this=0x7fff50006460) at sql/item.cc:364
  #7506 0x0000555555c768d4 in Item_sum_num::val_int (this=0x7fff50006460) at sql/item_sum.h:707
  #7507 0x0000555555c76a54 in Item_sum_int::val_real (this=0x7fff50006460) at sql/item_sum.h:721
  #7508 0x0000555555cd3a1c in Item::val_int_from_real (this=0x7fff50006460) at sql/item.cc:364
  #7509 0x0000555555c768d4 in Item_sum_num::val_int (this=0x7fff50006460) at sql/item_sum.h:707
  #7510 0x0000555555c76a54 in Item_sum_int::val_real (this=0x7fff50006460) at sql/item_sum.h:721
  #7511 0x0000555555e6b411 in Item_window_func::val_real (this=0x7fff5000d870) at sql/item_windowfunc.h:291
  #7512 0x0000555555ce1f40 in Item::save_in_field (this=0x7fff5000d870, field=0x7fff50012be0, no_conversions=true) at sql/item.cc:5843
  #7513 0x00005555559c2c54 in Item_result_field::save_in_result_field (this=0x7fff5000d870, no_conversions=true) at sql/item.h:2280
  #7514 0x0000555555aeb6bf in copy_funcs (func_ptr=0x7fff500126c8, thd=0x55555ab77458) at sql/sql_select.cc:23077
  #7515 0x0000555555ae2d01 in end_write (join=0x7fff5000f230, join_tab=0x7fff50010728, end_of_records=false) at sql/sql_select.cc:19520
  #7516 0x0000555555adffc1 in evaluate_join_record (join=0x7fff5000f230, join_tab=0x7fff500103e0, error=0) at sql/sql_select.cc:18388
  #7517 0x0000555555adf8b6 in sub_select (join=0x7fff5000f230, join_tab=0x7fff500103e0, end_of_records=false) at sql/sql_select.cc:18163

  is this normal? Can it happen with other val_XXX functions? 
  Should we use another way to prevent this by forcing
  Item_window_func::val_real() to return NULL at phase #1?
*/

class Item_sum_rank: public Item_sum_int
{
  longlong row_number; // just ROW_NUMBER()
  longlong cur_rank;   // current value
  
  List<Cached_item> orderby_fields;
public:
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

public:
  Item_sum_rank(THD *thd)
    : Item_sum_int(thd) {}

  enum Sumfunctype sum_func () const
  {
    return RANK_FUNC;
  }

  const char*func_name() const
  {
    return "rank";
  }
  
  void setup_window_func(THD *thd, Window_spec *window_spec);
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
  List<Cached_item> orderby_fields;
  /*
     XXX(cvicentiu) This class could potentially be implemented in the rank
     class, with a switch for the DENSE case.
  */
  void clear()
  {
    dense_rank= 1;
  }
  bool add();
  void update_field() {}
  longlong val_int()
  {
    return dense_rank;
  }

 public:
  Item_sum_dense_rank(THD *thd)
    : Item_sum_int(thd), dense_rank(0) {}
  enum Sumfunctype sum_func () const
  {
    return DENSE_RANK_FUNC;
  }

  const char*func_name() const
  {
    return "dense_rank";
  }

  void setup_window_func(THD *thd, Window_spec *window_spec);

};


/*
  @detail
  "The relative rank of a row R is defined as (RK-1)/(NR-1), where RK is 
  defined to be the RANK of R and NR is defined to be the number of rows in
  the window partition of R."

  Computation of this function requires two passes:
  - First pass to find #rows in the partition
  - Second pass to compute rank of current row and the value of the function
*/

class Item_sum_percent_rank: public Item_sum_num
{
  longlong rank;
  longlong partition_rows;

  void clear() {}
  bool add() { return false; }
  void update_field() {}

 public:
  Item_sum_percent_rank(THD *thd)
    : Item_sum_num(thd), rank(0), partition_rows(0) {}

  double val_real() { return 0; }

  enum Sumfunctype sum_func () const
  {
    return PERCENT_RANK_FUNC;
  }

  const char*func_name() const
  {
    return "percent_rank";
  }
  
  enum_field_types field_type() const { return MYSQL_TYPE_DOUBLE; }

};


/*
  @detail
  "The relative rank of a row R is defined as NP/NR, where 
  - NP is defined to be the number of rows preceding or peer with R in the 
    window ordering of the window partition of R
  - NR is defined to be the number of rows in the window partition of R.

  Just like with Item_sum_percent_rank, compuation of this function requires
  two passes.
*/

class Item_sum_cume_dist: public Item_sum_num
{
  longlong count;
  longlong partition_rows;

  void clear() {}
  bool add() { return false; }
  void update_field() {}

 public:
  Item_sum_cume_dist(THD *thd)
    : Item_sum_num(thd), count(0), partition_rows(0) {}

  double val_real() { return 0; }

  enum Sumfunctype sum_func () const
  {
    return CUME_DIST_FUNC;
  }

  const char*func_name() const
  {
    return "cume_dist";
  }

  enum_field_types field_type() const { return MYSQL_TYPE_DOUBLE; }
 
};


class Item_window_func : public Item_result_field
{
  /* Window function parameters as we've got them from the parser */
public:
  Item_sum *window_func;
  LEX_STRING *window_name;
public:
  Window_spec *window_spec;
  
  /*
    This stores the data bout the partition we're currently in.
    advance_window() uses this to tell when we've left one partition and
    entered another.
  */
  List<Cached_item> partition_fields;
public:
  Item_window_func(THD *thd, Item_sum *win_func, LEX_STRING *win_name)
    : Item_result_field(thd), window_func(win_func),
      window_name(win_name), window_spec(NULL), 
      force_return_blank(true),
      read_value_from_result_field(false) {}

  Item_window_func(THD *thd, Item_sum *win_func, Window_spec *win_spec)
    : Item_result_field(thd), window_func(win_func),
      window_name(NULL), window_spec(win_spec), 
      force_return_blank(true),
      read_value_from_result_field(false) {}

  /*
    Computation functions.
    TODO: consoder merging these with class Group_bound_tracker.
  */
  void setup_partition_border_check(THD *thd);

  void advance_window();
  int check_partition_bound();

  enum_field_types field_type() const { return window_func->field_type(); }
  enum Item::Type type() const { return Item::WINDOW_FUNC_ITEM; }
  
  /* 
    Window functions are very special functions, so val_() methods have
    special meaning for them:

    - Phase#1: we run the join and put its result into temporary table. For 
      window functions, we write NULL (or some other) values as placeholders.
      
    - Phase#2: executor does the scan in {PARTITION, ORDER BY} order of this 
      window function. It calls appropriate methods to inform the window
      function about rows entering/leaving the window. 
      It calls window_func->val_int() so that current window function value 
      can be saved and stored in the temp.table.

    - Phase#3: the temporary table is read and passed to query output. 
      However, Item_window_func still remains in the select list, so
      item_windowfunc->val_int() will be called.
      During Phase#3, read_value_from_result_field= true.
  */
public:
  // TODO: how to reset this for subquery re-execution??
  bool force_return_blank;
private:

  bool read_value_from_result_field;

public:
  void set_read_value_from_result_field() 
  {
    read_value_from_result_field= true;
  }

  double val_real() 
  {
    if (force_return_blank)
      return 0.0;
    return read_value_from_result_field? result_field->val_real() :
                                         window_func->val_real();
  }

  longlong val_int()
  { 
    if (force_return_blank)
      return 0;
    return read_value_from_result_field? result_field->val_int() : 
                                          window_func->val_int(); 
  }

  String* val_str(String* str)
  {
    if (force_return_blank)
      return str;
    return read_value_from_result_field? result_field->val_str(str) : 
                                         window_func->val_str(str);
  }

  my_decimal* val_decimal(my_decimal* dec)
  { 
    if (force_return_blank)
    {
      my_decimal_set_zero(dec);
      return dec;
    }
    return read_value_from_result_field? result_field->val_decimal(dec) : 
                                         window_func->val_decimal(dec);
  }

  void split_sum_func(THD *thd, Ref_ptr_array ref_pointer_array,
                              List<Item> &fields, uint flags);
  void fix_length_and_dec()
  {
    window_func->fix_length_and_dec();
  }

  const char* func_name() const { return "WF"; }

  bool fix_fields(THD *thd, Item **ref);
  
  bool resolve_window_name(THD *thd);

};


#endif /* ITEM_WINDOWFUNC_INCLUDED */
