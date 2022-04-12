/*
   Copyright (c) 2016, 2020, MariaDB

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

#ifndef ITEM_WINDOWFUNC_INCLUDED
#define ITEM_WINDOWFUNC_INCLUDED

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

  const Type_handler *type_handler() const override
  { return &type_handler_slonglong; }

  void clear() override
  {
    count= 0;
  }

  bool add() override
  {
    count++;
    return false;
  }

  void reset_field() override { DBUG_ASSERT(0); }
  void update_field() override {}

  enum Sumfunctype sum_func() const override
  {
    return ROW_NUMBER_FUNC;
  }

  longlong val_int() override
  {
    return count;
  }
  LEX_CSTRING func_name_cstring() const override
  {
    static LEX_CSTRING name= {STRING_WITH_LEN("row_number") };
    return name;
  }

  Item *get_copy(THD *thd) override
  { return get_item_copy<Item_sum_row_number>(thd, this); }
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

  const Type_handler *type_handler() const override
  { return &type_handler_slonglong; }

  void clear() override
  {
    /* This is called on partition start */
    cur_rank= 1;
    row_number= 0;
  }

  bool add() override;

  longlong val_int() override
  {
    return cur_rank;
  }

  void reset_field() override { DBUG_ASSERT(0); }
  void update_field() override {}

  enum Sumfunctype sum_func () const override
  {
    return RANK_FUNC;
  }

  LEX_CSTRING func_name_cstring() const override
  {
    static LEX_CSTRING name= {STRING_WITH_LEN("rank") };
    return name;
  }

  void setup_window_func(THD *thd, Window_spec *window_spec) override;

  void cleanup() override
  {
    if (peer_tracker)
    {
      delete peer_tracker;
      peer_tracker= NULL;
    }
    Item_sum_int::cleanup();
  }
  Item *get_copy(THD *thd) override
  { return get_item_copy<Item_sum_rank>(thd, this); }
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
  void clear() override
  {
    dense_rank= 0;
    first_add= true;
  }
  bool add() override;
  void reset_field() override { DBUG_ASSERT(0); }
  void update_field() override {}
  longlong val_int() override
  {
    return dense_rank;
  }

  Item_sum_dense_rank(THD *thd)
    : Item_sum_int(thd), dense_rank(0), first_add(true), peer_tracker(NULL) {}
  const Type_handler *type_handler() const override
  { return &type_handler_slonglong; }
  enum Sumfunctype sum_func () const override
  {
    return DENSE_RANK_FUNC;
  }

  LEX_CSTRING func_name_cstring() const override
  {
    static LEX_CSTRING name= {STRING_WITH_LEN("dense_rank") };
    return name;
  }

  void setup_window_func(THD *thd, Window_spec *window_spec) override;

  void cleanup() override
  {
    if (peer_tracker)
    {
      delete peer_tracker;
      peer_tracker= NULL;
    }
    Item_sum_int::cleanup();
  }
  Item *get_copy(THD *thd) override
  { return get_item_copy<Item_sum_dense_rank>(thd, this); }
};

class Item_sum_hybrid_simple : public Item_sum_hybrid
{
 public:
  Item_sum_hybrid_simple(THD *thd, Item *arg):
   Item_sum_hybrid(thd, arg),
   value(NULL)
  { }

  Item_sum_hybrid_simple(THD *thd, Item *arg1, Item *arg2):
   Item_sum_hybrid(thd, arg1, arg2),
   value(NULL)
  { }

  bool add() override;
  bool fix_fields(THD *, Item **) override;
  bool fix_length_and_dec(THD *thd) override;
  void setup_hybrid(THD *thd, Item *item);
  double val_real() override;
  longlong val_int() override;
  my_decimal *val_decimal(my_decimal *) override;
  void reset_field() override;
  String *val_str(String *) override;
  bool val_native(THD *thd, Native *to) override;
  bool get_date(THD *thd, MYSQL_TIME *ltime, date_mode_t fuzzydate) override;
  const Type_handler *type_handler() const override
  { return Type_handler_hybrid_field_type::type_handler(); }
  void update_field() override;
  Field *create_tmp_field(MEM_ROOT *root, bool group, TABLE *table) override;
  void clear() override
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


  enum Sumfunctype sum_func () const override
  {
    return FIRST_VALUE_FUNC;
  }

  LEX_CSTRING func_name_cstring() const override
  {
    static LEX_CSTRING name= {STRING_WITH_LEN("first_value") };
    return name;
  }

  Item *get_copy(THD *thd) override
  { return get_item_copy<Item_sum_first_value>(thd, this); }
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

  enum Sumfunctype sum_func() const override
  {
    return LAST_VALUE_FUNC;
  }

  LEX_CSTRING func_name_cstring() const override
  {
    static LEX_CSTRING name= {STRING_WITH_LEN("last_value") };
    return name;
  }

  Item *get_copy(THD *thd) override
  { return get_item_copy<Item_sum_last_value>(thd, this); }
};


class Item_sum_nth_value : public Item_sum_hybrid_simple
{
 public:
  Item_sum_nth_value(THD *thd, Item *arg_expr, Item* offset_expr) :
    Item_sum_hybrid_simple(thd, arg_expr, offset_expr) {}

  enum Sumfunctype sum_func() const override
  {
    return NTH_VALUE_FUNC;
  }

  LEX_CSTRING func_name_cstring() const override
  {
    static LEX_CSTRING name= {STRING_WITH_LEN("nth_value") };
    return name;
  }

  Item *get_copy(THD *thd) override
  { return get_item_copy<Item_sum_nth_value>(thd, this); }
};


class Item_sum_lead : public Item_sum_hybrid_simple
{
 public:
  Item_sum_lead(THD *thd, Item *arg_expr, Item* offset_expr) :
    Item_sum_hybrid_simple(thd, arg_expr, offset_expr) {}

  enum Sumfunctype sum_func() const override
  {
    return LEAD_FUNC;
  }

  LEX_CSTRING func_name_cstring() const override
  {
    static LEX_CSTRING name= {STRING_WITH_LEN("lead") };
    return name;
  }

  Item *get_copy(THD *thd) override
  { return get_item_copy<Item_sum_lead>(thd, this); }
};


class Item_sum_lag : public Item_sum_hybrid_simple
{
 public:
  Item_sum_lag(THD *thd, Item *arg_expr, Item* offset_expr) :
    Item_sum_hybrid_simple(thd, arg_expr, offset_expr) {}

  enum Sumfunctype sum_func() const override
  {
    return LAG_FUNC;
  }

  LEX_CSTRING func_name_cstring() const override
  {
    static LEX_CSTRING name= {STRING_WITH_LEN("lag") };
    return name;
  }

  Item *get_copy(THD *thd) override
  { return get_item_copy<Item_sum_lag>(thd, this); }
};


class Partition_row_count
{
public:
  Partition_row_count() :partition_row_count_(0) { }
  void set_partition_row_count(ulonglong count)
  {
    partition_row_count_ = count;
  }
  double calc_val_real(bool *null_value,
                       ulonglong current_row_count)
  {
    if ((*null_value= (partition_row_count_ == 0)))
      return 0;
    return static_cast<double>(current_row_count) / partition_row_count_;
  }
protected:
  longlong get_row_count() { return partition_row_count_; }
  ulonglong partition_row_count_;
};


class Current_row_count
{
public:
  Current_row_count() :current_row_count_(0) { }
protected:
  ulonglong get_row_number() { return current_row_count_ ; }
  ulonglong current_row_count_;
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
class Item_sum_percent_rank: public Item_sum_double,
                             public Partition_row_count
{
 public:
  Item_sum_percent_rank(THD *thd)
    : Item_sum_double(thd), cur_rank(1), peer_tracker(NULL) {}

  longlong val_int() override
  {
   /*
      Percent rank is a real value so calling the integer value should never
      happen. It makes no sense as it gets truncated to either 0 or 1.
   */
    DBUG_ASSERT(0);
    return 0;
  }

  double val_real() override
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

  enum Sumfunctype sum_func () const override
  {
    return PERCENT_RANK_FUNC;
  }

  LEX_CSTRING func_name_cstring() const override
  {
    static LEX_CSTRING name= {STRING_WITH_LEN("percent_rank") };
    return name;
  }

  void update_field() override {}

  void clear() override
  {
    cur_rank= 1;
    row_number= 0;
  }
  bool add() override;
  const Type_handler *type_handler() const override
  { return &type_handler_double; }

  bool fix_length_and_dec(THD *thd) override
  {
    decimals = 10;  // TODO-cvicentiu find out how many decimals the standard
                    // requires.
    return FALSE;
  }

  void setup_window_func(THD *thd, Window_spec *window_spec) override;

  void reset_field() override { DBUG_ASSERT(0); }

  void set_partition_row_count(ulonglong count) override
  {
    Partition_row_count::set_partition_row_count(count);
  }

  Item *get_copy(THD *thd) override
  { return get_item_copy<Item_sum_percent_rank>(thd, this); }

 private:
  longlong cur_rank;   // Current rank of the current row.
  longlong row_number; // Value if this were ROW_NUMBER() function.

  Group_bound_tracker *peer_tracker;

  void cleanup() override
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

class Item_sum_cume_dist: public Item_sum_double,
                          public Partition_row_count,
                          public Current_row_count
{
 public:
  Item_sum_cume_dist(THD *thd) :Item_sum_double(thd) { }
  Item_sum_cume_dist(THD *thd, Item *arg) :Item_sum_double(thd, arg) { }

  double val_real() override
  {
    return calc_val_real(&null_value, current_row_count_);
  }

  bool add() override
  {
    current_row_count_++;
    return false;
  }

  enum Sumfunctype sum_func() const override
  {
    return CUME_DIST_FUNC;
  }

  void clear() override
  {
    current_row_count_= 0;
    partition_row_count_= 0;
  }

  LEX_CSTRING func_name_cstring() const override
  {
    static LEX_CSTRING name= {STRING_WITH_LEN("cume_dist") };
    return name;
  }

  void update_field() override {}
  const Type_handler *type_handler() const override
  { return &type_handler_double; }

  bool fix_length_and_dec(THD *thd) override
  {
    decimals = 10;  // TODO-cvicentiu find out how many decimals the standard
                    // requires.
    return FALSE;
  }
  
  void reset_field() override { DBUG_ASSERT(0); }

  void set_partition_row_count(ulonglong count) override
  {
    Partition_row_count::set_partition_row_count(count);
  }

  Item *get_copy(THD *thd) override
  { return get_item_copy<Item_sum_cume_dist>(thd, this); }

};

class Item_sum_ntile : public Item_sum_int,
                       public Partition_row_count,
                       public Current_row_count
{
 public:
  Item_sum_ntile(THD* thd, Item* num_quantiles_expr) :
    Item_sum_int(thd, num_quantiles_expr), n_old_val_(0)
  { }

  longlong val_int() override
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

  bool add() override
  {
    current_row_count_++;
    return false;
  }

  enum Sumfunctype sum_func() const override
  {
    return NTILE_FUNC;
  }

  void clear() override
  {
    current_row_count_= 0;
    partition_row_count_= 0;
    n_old_val_= 0;
  }

  LEX_CSTRING func_name_cstring() const override
  {
    static LEX_CSTRING name= {STRING_WITH_LEN("ntile") };
    return name;
  }

  void update_field() override {}

  const Type_handler *type_handler() const override
  { return &type_handler_slonglong; }

  void reset_field() override { DBUG_ASSERT(0); }

  void set_partition_row_count(ulonglong count) override
  {
    Partition_row_count::set_partition_row_count(count);
  }

  Item *get_copy(THD *thd) override
  { return get_item_copy<Item_sum_ntile>(thd, this); }

 private:
  longlong get_num_quantiles() { return args[0]->val_int(); }
  ulonglong n_old_val_;
};

class Item_sum_percentile_disc : public Item_sum_num,
                                 public Type_handler_hybrid_field_type,
                                 public Partition_row_count,
                                 public Current_row_count
{
public:
  Item_sum_percentile_disc(THD *thd, Item* arg) : Item_sum_num(thd, arg),
                           Type_handler_hybrid_field_type(&type_handler_slonglong),
                           value(NULL), val_calculated(FALSE), first_call(TRUE),
                           prev_value(0), order_item(NULL){}

  double val_real() override
  {
    if (get_row_count() == 0 || get_arg(0)->is_null())
    {
      null_value= true;
      return 0;
    }
    null_value= false;
    return value->val_real();
  }

  longlong val_int() override
  {
    if (get_row_count() == 0 || get_arg(0)->is_null())
    {
      null_value= true;
      return 0;
    }
    null_value= false;
    return value->val_int();
  }

  my_decimal* val_decimal(my_decimal* dec) override
  {
    if (get_row_count() == 0 || get_arg(0)->is_null())
    {
      null_value= true;
      return 0;
    }
    null_value= false;
    return value->val_decimal(dec);
  }

  String* val_str(String *str) override
  {
    if (get_row_count() == 0 || get_arg(0)->is_null())
    {
      null_value= true;
      return 0;
    }
    null_value= false;
    return value->val_str(str);
  }

  bool get_date(THD *thd, MYSQL_TIME *ltime, date_mode_t fuzzydate) override
  {
    if (get_row_count() == 0 || get_arg(0)->is_null())
    {
      null_value= true;
      return true;
    }
    null_value= false;
    return value->get_date(thd, ltime, fuzzydate);
  }

  bool val_native(THD *thd, Native *to) override
  {
    if (get_row_count() == 0 || get_arg(0)->is_null())
    {
      null_value= true;
      return true;
    }
    null_value= false;
    return value->val_native(thd, to);
  }

  bool add() override
  {
    Item *arg= get_arg(0);
    if (arg->is_null())
      return false;

    if (first_call)
    {
      prev_value= arg->val_real();
      if (prev_value > 1 || prev_value < 0)
      {
        my_error(ER_ARGUMENT_OUT_OF_RANGE, MYF(0), func_name());
        return true;
      }
      first_call= false;
    }

    double arg_val= arg->val_real();

    if (prev_value != arg_val)
    {
      my_error(ER_ARGUMENT_NOT_CONSTANT, MYF(0), func_name());
      return true;
    }

    if (val_calculated)
      return false;

    value->store(order_item);
    value->cache_value();
    if (value->null_value)
      return false;

    current_row_count_++;
    double val= calc_val_real(&null_value, current_row_count_);

    if (val >= prev_value && !val_calculated)
      val_calculated= true;
    return false;
  }

  enum Sumfunctype sum_func() const override
  {
    return PERCENTILE_DISC_FUNC;
  }

  void clear() override
  {
    val_calculated= false;
    first_call= true;
    value->clear();
    partition_row_count_= 0;
    current_row_count_= 0;
  }

  LEX_CSTRING func_name_cstring() const override
  {
    static LEX_CSTRING name= {STRING_WITH_LEN("percentile_disc") };
    return name;
  }

  void update_field() override {}
  const Type_handler *type_handler() const override
  {return Type_handler_hybrid_field_type::type_handler();}

  bool fix_length_and_dec(THD *thd) override
  {
    decimals = 10;  // TODO-cvicentiu find out how many decimals the standard
                    // requires.
    return FALSE;
  }

  void reset_field() override { DBUG_ASSERT(0); }

  void set_partition_row_count(ulonglong count) override
  {
    Partition_row_count::set_partition_row_count(count);
  }

  Item *get_copy(THD *thd) override
  { return get_item_copy<Item_sum_percentile_disc>(thd, this); }
  void setup_window_func(THD *thd, Window_spec *window_spec) override;
  void setup_hybrid(THD *thd, Item *item);
  bool fix_fields(THD *thd, Item **ref) override;

private:
  Item_cache *value;
  bool val_calculated;
  bool first_call;
  double prev_value;
  Item *order_item;
};

class Item_sum_percentile_cont : public Item_sum_double,
                                 public Partition_row_count,
                                 public Current_row_count
{
public:
  Item_sum_percentile_cont(THD *thd, Item* arg) : Item_sum_double(thd, arg),
                           floor_value(NULL), ceil_value(NULL), first_call(TRUE),prev_value(0),
                           ceil_val_calculated(FALSE), floor_val_calculated(FALSE), order_item(NULL){}

  double val_real() override
  {
    if (get_row_count() == 0 || get_arg(0)->is_null())
    {
      null_value= true;
      return 0;
    }
    null_value= false;
    double val= 1 + prev_value * (get_row_count()-1);

    /*
      Applying the formula to get the value
      If (CRN = FRN = RN) then the result is (value of expression from row at RN)
      Otherwise the result is
      (CRN - RN) * (value of expression for row at FRN) +
      (RN - FRN) * (value of expression for row at CRN)
    */

    if(ceil(val) == floor(val))
      return floor_value->val_real();

    double ret_val=  ((val - floor(val)) * ceil_value->val_real()) +
                  ((ceil(val) - val) * floor_value->val_real());

    return ret_val;
  }

  bool add() override
  {
    Item *arg= get_arg(0);
    if (arg->is_null())
      return false;

    if (first_call)
    {
      first_call= false;
      prev_value= arg->val_real();
      if (prev_value > 1 || prev_value < 0)
      {
        my_error(ER_ARGUMENT_OUT_OF_RANGE, MYF(0), func_name());
        return true;
      }
    }

    double arg_val= arg->val_real();
    if (prev_value != arg_val)
    {
      my_error(ER_ARGUMENT_NOT_CONSTANT, MYF(0), func_name());
      return true;
    }

    if (!floor_val_calculated)
    {
      floor_value->store(order_item);
      floor_value->cache_value();
      if (floor_value->null_value)
        return false;
    }
    if (floor_val_calculated && !ceil_val_calculated)
    {
      ceil_value->store(order_item);
      ceil_value->cache_value();
      if (ceil_value->null_value)
        return false;
    }

    current_row_count_++;
    double val= 1 + prev_value * (get_row_count()-1);

    if (!floor_val_calculated && get_row_number() == floor(val))
      floor_val_calculated= true;

    if (!ceil_val_calculated && get_row_number() == ceil(val))
      ceil_val_calculated= true;
    return false;
  }

  enum Sumfunctype sum_func() const override
  {
    return PERCENTILE_CONT_FUNC;
  }

  void clear() override
  {
    first_call= true;
    floor_value->clear();
    ceil_value->clear();
    floor_val_calculated= false;
    ceil_val_calculated= false;
    partition_row_count_= 0;
    current_row_count_= 0;
  }

  LEX_CSTRING func_name_cstring() const override
  {
    static LEX_CSTRING name= {STRING_WITH_LEN("percentile_cont") };
    return name;
  }
  void update_field() override {}

  bool fix_length_and_dec(THD *thd) override
  {
    decimals = 10;  // TODO-cvicentiu find out how many decimals the standard
                    // requires.
    return FALSE;
  }

  void reset_field() override { DBUG_ASSERT(0); }

  void set_partition_row_count(ulonglong count) override
  {
    Partition_row_count::set_partition_row_count(count);
  }

  Item *get_copy(THD *thd) override
  { return get_item_copy<Item_sum_percentile_cont>(thd, this); }
  void setup_window_func(THD *thd, Window_spec *window_spec) override;
  void setup_hybrid(THD *thd, Item *item);
  bool fix_fields(THD *thd, Item **ref) override;

private:
  Item_cache *floor_value;
  Item_cache *ceil_value;
  bool first_call;
  double prev_value;
  bool ceil_val_calculated;
  bool floor_val_calculated;
  Item *order_item;
};




class Item_window_func : public Item_func_or_sum
{
  /* Window function parameters as we've got them from the parser */
public:
  LEX_CSTRING *window_name;
public:
  Window_spec *window_spec;
  
public:
  Item_window_func(THD *thd, Item_sum *win_func, LEX_CSTRING *win_name)
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

  void update_used_tables() override;

  /*
    This is used by filesort to mark the columns it needs to read (because they
    participate in the sort criteria and/or row retrieval. Window functions can
    only be used in sort criteria).

    Sorting by window function value is only done after the window functions
    have been computed. In that case, window function will need to read its
    temp.table field. In order to allow that, mark that field in the read_set.
  */
  bool register_field_in_read_map(void *arg) override
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
    case Item_sum::PERCENTILE_CONT_FUNC:
    case Item_sum::PERCENTILE_DISC_FUNC:
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
    case Item_sum::PERCENTILE_CONT_FUNC:
    case Item_sum::PERCENTILE_DISC_FUNC:
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
    case Item_sum::PERCENTILE_CONT_FUNC:
    case Item_sum::PERCENTILE_DISC_FUNC:
      return true;
    default: 
      return false;
    }
  }  

  bool only_single_element_order_list() const
  {
    switch (window_func()->sum_func()){
    case Item_sum::PERCENTILE_CONT_FUNC:
    case Item_sum::PERCENTILE_DISC_FUNC:
      return true;
    default:
      return false;
    }
  }

  bool check_result_type_of_order_item();



  /*
    Computation functions.
    TODO: consoder merging these with class Group_bound_tracker.
  */
  void setup_partition_border_check(THD *thd);

  const Type_handler *type_handler() const override
  {
    return ((Item_sum *) args[0])->type_handler();
  }
  enum Item::Type type() const override { return Item::WINDOW_FUNC_ITEM; }

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
  void print_for_percentile_functions(String *str, enum_query_type query_type);

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

  bool is_null() override
  {
    if (force_return_blank)
      return true;

    if (read_value_from_result_field)
      return result_field->is_null();

    return window_func()->is_null();
  }

  double val_real() override 
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

  longlong val_int() override
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

  String* val_str(String* str) override
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

  bool val_native(THD *thd, Native *to) override
  {
    if (force_return_blank)
      return null_value= true;
    if (read_value_from_result_field)
      return val_native_from_field(result_field, to);
    return val_native_from_item(thd, window_func(), to);
  }

  my_decimal* val_decimal(my_decimal* dec) override
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

  bool get_date(THD *thd, MYSQL_TIME *ltime, date_mode_t fuzzydate) override
  {
    bool res;
    if (force_return_blank)
    {
      null_value= true;
      res= true;
    }
    else if (read_value_from_result_field)
    {
      if ((null_value= result_field->is_null()))
        res= true;
      else
        res= result_field->get_date(ltime, fuzzydate);
    }
    else
    {
      res= window_func()->get_date(thd, ltime, fuzzydate);
      null_value= window_func()->null_value;
    }
    return res;
  }

  void split_sum_func(THD *thd, Ref_ptr_array ref_pointer_array,
                              List<Item> &fields, uint flags) override;

  bool fix_length_and_dec(THD *thd) override
  {
    Type_std_attributes::set(window_func());
    return FALSE;
  }

  LEX_CSTRING func_name_cstring() const override
  {
    static LEX_CSTRING name= {STRING_WITH_LEN("WF") };
    return name;
  }

  bool fix_fields(THD *thd, Item **ref) override;

  bool resolve_window_name(THD *thd);
  
  void print(String *str, enum_query_type query_type) override;

 Item *get_copy(THD *thd) override { return 0; }

};

#endif /* ITEM_WINDOWFUNC_INCLUDED */
