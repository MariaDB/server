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
*/

class Item_sum_rank: public Item_sum_int
{
protected:
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

/* TODO-cvicentiu
 * Perhaps this is overengineering, but I would like to decouple the 2-pass
 * algorithm from the specific action that must be performed during the
 * first pass. The second pass can make use of the "add" function from the
 * Item_sum_<window_function>.
 */

/*
   This class represents a generic interface for window functions that need
   to store aditional information. Such window functions include percent_rank
   and cume_dist.
*/
class Window_context
{
 public:
  virtual void add_field_to_context(Field* field) = 0;
  virtual void reset() = 0;
  virtual ~Window_context() {};
};

/*
   A generic interface that specifies the datatype that the context represents.
*/
template <typename T>
class Window_context_getter
{
 protected:
  virtual T get_field_context(const Field* field) = 0;
  virtual ~Window_context_getter() {};
};

/*
   A window function context representing the number of rows that are present
   with a partition. Because the number of rows is not dependent of the
   specific value within the current field, we ignore the parameter
   in this case.
*/
class Window_context_row_count :
  public Window_context, Window_context_getter<ulonglong>
{
 public:
  Window_context_row_count() : num_rows_(0) {};

  void add_field_to_context(Field* field __attribute__((unused)))
  {
    num_rows_++;
  }

  void reset()
  {
    num_rows_= 0;
  }

  ulonglong get_field_context(const Field* field __attribute__((unused)))
  {
    return num_rows_;
  }
 private:
  ulonglong num_rows_;
};

class Window_context_row_and_group_count :
  public Window_context, Window_context_getter<std::pair<ulonglong, ulonglong> >
{
 public:
  Window_context_row_and_group_count(void * group_list) {}
};

/*
  An abstract class representing an item that holds a context.
*/
class Item_context
{
 public:
  Item_context() : context_(NULL) {}
  Window_context* get_window_context() { return context_; }

  virtual bool create_window_context() = 0;
  virtual void delete_window_context() = 0;

 protected:
  Window_context* context_;
};

/*
  A base window function (aggregate) that also holds a context.

  NOTE: All two pass window functions need to implement
  this interface.
*/
class Item_sum_window_with_context : public Item_sum_num,
                                     public Item_context
{
 public:
  Item_sum_window_with_context(THD *thd)
   : Item_sum_num(thd), Item_context() {}
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
class Item_sum_percent_rank: public Item_sum_window_with_context,
                             public Window_context_row_count
{
 public:
  Item_sum_percent_rank(THD *thd)
    : Item_sum_window_with_context(thd), cur_rank(1) {}

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
   if (!get_context_())
   {
     // Calling this kind of function with a context makes no sense.
     DBUG_ASSERT(0);
     return 0;
   }

   longlong partition_rows = get_context_()->get_field_context(result_field);
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

  bool create_window_context()
  {
    // TODO-cvicentiu: Currently this means we must make sure to delete
    // the window context. We can potentially allocate this on the THD memroot.
    // At the same time, this is only necessary for a small portion of the
    // query execution and it does not make sense to keep it for all of it.
    context_ = new Window_context_row_count();
    if (context_ == NULL)
      return true;
    return false;
  }

  void delete_window_context()
  {
    if (context_)
      delete get_context_();
    context_ = NULL;
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

  void fix_length_and_dec()
  {
    decimals = 10;  // TODO-cvicentiu find out how many decimals the standard
                    // requires.
  }

  void setup_window_func(THD *thd, Window_spec *window_spec);

 private:
  longlong cur_rank;   // Current rank of the current row.
  longlong row_number; // Value if this were ROW_NUMBER() function.

  List<Cached_item> orderby_fields;

  /* Helper function so that we don't cast the context every time. */
  Window_context_row_count* get_context_()
  {
    return static_cast<Window_context_row_count *>(context_);
  }
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

class Item_sum_cume_dist: public Item_sum_percent_rank
{
 public:
  Item_sum_cume_dist(THD *thd)
    : Item_sum_percent_rank(thd) {}

  double val_real() { return 0; }

  enum Sumfunctype sum_func () const
  {
    return CUME_DIST_FUNC;
  }

  const char*func_name() const
  {
    return "cume_dist";
  }
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
    double res;
    if (force_return_blank)
    {
      res= 0.0;
      null_value= false;
    }
    else if (read_value_from_result_field)
    {
      res= result_field->val_real();
      null_value= result_field->is_null();
    }
    else
    {
      res= window_func->val_real();
      null_value= window_func->null_value;
    }
    return res;
  }

  longlong val_int()
  {
    longlong res;
    if (force_return_blank)
    {
      res= 0;
      null_value= false;
    }
    else if (read_value_from_result_field)
    {
      res= result_field->val_int();
      null_value= result_field->is_null();
    }
    else
    {
      res= window_func->val_int();
      null_value= window_func->null_value;
    }
    return res;
  }

  String* val_str(String* str)
  {
    String *res;
    if (force_return_blank)
    {
      null_value= false;
      str->length(0);
      res= str;
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
      res= window_func->val_str(str);
      null_value= window_func->null_value;
    }
    return res;
  }

  my_decimal* val_decimal(my_decimal* dec)
  {
    my_decimal *res;
    if (force_return_blank)
    {
      my_decimal_set_zero(dec);
      null_value= false;
      res= dec;
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
      res= window_func->val_decimal(dec);
      null_value= window_func->null_value;
    }
    return res;
  }

  void split_sum_func(THD *thd, Ref_ptr_array ref_pointer_array,
                              List<Item> &fields, uint flags);
  void fix_length_and_dec()
  {
    decimals = window_func->decimals;
  }

  const char* func_name() const { return "WF"; }

  bool fix_fields(THD *thd, Item **ref);

  bool resolve_window_name(THD *thd);
};

#endif /* ITEM_WINDOWFUNC_INCLUDED */
