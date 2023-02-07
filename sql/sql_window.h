/*
   Copyright (c) 2016, 2022 MariaDB

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

#ifndef SQL_WINDOW_INCLUDED
#define SQL_WINDOW_INCLUDED

#include "filesort.h"

class Item_window_func;

/*
  Window functions module. 
  
  Each instance of window function has its own element in SELECT_LEX::window_specs.
*/


class Window_frame_bound : public Sql_alloc
{

public:
 
  enum Bound_precedence_type
  {
    PRECEDING,
    CURRENT,           // Used for CURRENT ROW window frame bounds
    FOLLOWING
  };

  Bound_precedence_type precedence_type;
 

  /* 
    For UNBOUNDED PRECEDING / UNBOUNDED FOLLOWING window frame bounds
    precedence type is seto to PRECEDING / FOLLOWING and
    offset is set to NULL. 
    The offset is not meaningful with precedence type CURRENT 
  */
  Item *offset;

  Window_frame_bound(Bound_precedence_type prec_type,
                     Item *offset_val)
    : precedence_type(prec_type), offset(offset_val) {}

  bool is_unbounded() { return offset == NULL; }

  void print(String *str, enum_query_type query_type);

};


class Window_frame : public Sql_alloc
{
  
public:

  enum Frame_units
  {
    UNITS_ROWS,
    UNITS_RANGE
  };

  enum Frame_exclusion
  {
    EXCL_NONE,
    EXCL_CURRENT_ROW,
    EXCL_GROUP,
    EXCL_TIES
  };

  Frame_units units;

  Window_frame_bound *top_bound;

  Window_frame_bound *bottom_bound;

  Frame_exclusion exclusion;

  Window_frame(Frame_units win_frame_units,
               Window_frame_bound *win_frame_top_bound,
               Window_frame_bound *win_frame_bottom_bound,
               Frame_exclusion win_frame_exclusion)
    : units(win_frame_units), top_bound(win_frame_top_bound),
      bottom_bound(win_frame_bottom_bound), exclusion(win_frame_exclusion) {}

  bool check_frame_bounds();

  void print(String *str, enum_query_type query_type);

};

class Window_spec : public Sql_alloc
{
  bool window_names_are_checked;
 public:
  virtual ~Window_spec() = default;

  LEX_CSTRING *window_ref;

  SQL_I_List<ORDER> *partition_list;
  SQL_I_List<ORDER> *save_partition_list;

  SQL_I_List<ORDER> *order_list;
  SQL_I_List<ORDER> *save_order_list;

  Window_frame *window_frame;

  Window_spec *referenced_win_spec;

  /*
    Window_spec objects are numbered by the number of their appearance in the
    query. This is used by compare_order_elements() to provide a predictable
    ordering of PARTITION/ORDER BY clauses.
  */
  int win_spec_number;

  Window_spec(LEX_CSTRING *win_ref, SQL_I_List<ORDER> *part_list,
              SQL_I_List<ORDER> *ord_list, Window_frame *win_frame)
    : window_names_are_checked(false), window_ref(win_ref),
      partition_list(part_list), save_partition_list(NULL),
      order_list(ord_list), save_order_list(NULL),
      window_frame(win_frame), referenced_win_spec(NULL) {}

  virtual const char *name() { return NULL; }

  bool check_window_names(List_iterator_fast<Window_spec> &it);

  const char *window_reference()
  {
    return window_ref ? window_ref->str : NULL;
  }

  void join_partition_and_order_lists()
  {
    *(partition_list->next)= order_list->first;
  }

  void disjoin_partition_and_order_lists()
  {
    *(partition_list->next)= NULL;
  }

  void print(String *str, enum_query_type query_type);
  void print_order(String *str, enum_query_type query_type);
  void print_partition(String *str, enum_query_type query_type);

};

class Window_def : public Window_spec
{
 public:

  LEX_CSTRING *window_name;

  Window_def(LEX_CSTRING *win_name,
             LEX_CSTRING *win_ref, 
             SQL_I_List<ORDER> *part_list,
             SQL_I_List<ORDER> *ord_list,
             Window_frame *win_frame) 
    : Window_spec(win_ref, part_list, ord_list, win_frame),
      window_name(win_name) {}
 
  const char *name() { return window_name->str; }

};

int setup_windows(THD *thd, Ref_ptr_array ref_pointer_array, TABLE_LIST *tables,
	          List<Item> &fields, List<Item> &all_fields, 
                  List<Window_spec> &win_specs, List<Item_window_func> &win_funcs);


//////////////////////////////////////////////////////////////////////////////
// Classes that make window functions computation a part of SELECT's query plan
//////////////////////////////////////////////////////////////////////////////

class Frame_cursor;
/*
  This handles computation of one window function.

  Currently, we make a spearate filesort() call for each window function.
*/

class Window_func_runner : public Sql_alloc
{
public:
  /* Add the function to be computed during the execution pass  */
  bool add_function_to_run(Item_window_func *win_func);

  /* Compute and fill the fields in the table. */
  bool exec(THD *thd, TABLE *tbl, SORT_INFO *filesort_result);

private:
  /* A list of window functions for which this Window_func_runner will compute
     values during the execution phase. */
  List<Item_window_func> window_functions;
};


/*
  Represents a group of window functions that require the same sorting of 
  rows and so share the filesort() call.

*/

class Window_funcs_sort : public Sql_alloc
{
public:
  bool setup(THD *thd, SQL_SELECT *sel, List_iterator<Item_window_func> &it,
             st_join_table *join_tab);
  bool exec(JOIN *join, bool keep_filesort_result);
  void cleanup() { delete filesort; }

  friend class Window_funcs_computation;

private:
  Window_func_runner runner;

  /* Window functions can be computed over this sorting */
  Filesort *filesort;
};


struct st_join_table;
class Explain_aggr_window_funcs;

/*
  This is a "window function computation phase": a single object of this class
  takes care of computing all window functions in a SELECT.

  - JOIN optimizer is exected to call setup() during query optimization.
  - JOIN::exec() should call exec() once it has collected join output in a
    temporary table.
*/

class Window_funcs_computation : public Sql_alloc
{
  List<Window_funcs_sort> win_func_sorts;
public:
  bool setup(THD *thd, List<Item_window_func> *window_funcs, st_join_table *tab);
  bool exec(JOIN *join, bool keep_last_filesort_result);

  Explain_aggr_window_funcs *save_explain_plan(MEM_ROOT *mem_root, bool is_analyze);
  void cleanup();
};


#endif /* SQL_WINDOW_INCLUDED */
