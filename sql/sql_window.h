
#ifndef SQL_WINDOW_INCLUDED
#define SQL_WINDOW_INCLUDED

#include "my_global.h"
#include "item.h"

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

};

class Window_spec : public Sql_alloc
{
 public:

  LEX_STRING *window_ref;

  SQL_I_List<ORDER> *partition_list;

  SQL_I_List<ORDER> *order_list;

  Window_frame *window_frame;

  Window_spec *referenced_win_spec;

  Window_spec(LEX_STRING *win_ref, 
              SQL_I_List<ORDER> *part_list,
              SQL_I_List<ORDER> *ord_list,
              Window_frame *win_frame)
    : window_ref(win_ref), partition_list(part_list), order_list(ord_list),
    window_frame(win_frame), referenced_win_spec(NULL) {}

  virtual char *name() { return NULL; }

  bool check_window_names(List_iterator_fast<Window_spec> &it);

  char *window_reference() { return window_ref ? window_ref->str : NULL; }
};

class Window_def : public Window_spec
{
 public:

  LEX_STRING *window_name;

  Window_def(LEX_STRING *win_name,
             LEX_STRING *win_ref, 
             SQL_I_List<ORDER> *part_list,
             SQL_I_List<ORDER> *ord_list,
             Window_frame *win_frame) 
    : Window_spec(win_ref, part_list, ord_list, win_frame),
      window_name(win_name) {}
 
  char *name() { return window_name->str; }

};

int setup_windows(THD *thd, Ref_ptr_array ref_pointer_array, TABLE_LIST *tables,
	          List<Item> &fields, List<Item> &all_fields, 
                  List<Window_spec> &win_specs);

#endif /* SQL_WINDOW_INCLUDED */
