#include "sql_select.h"
#include "sql_list.h"
#include "item_windowfunc.h"
#include "filesort.h"
#include "sql_base.h"
#include "sql_window.h"
#include "my_dbug.h"


bool
Window_spec::check_window_names(List_iterator_fast<Window_spec> &it)
{
  if (window_names_are_checked)
    return false;
  char *name= this->name();
  char *ref_name= window_reference();
  it.rewind();
  Window_spec *win_spec;
  while((win_spec= it++) && win_spec != this)
  {
    char *win_spec_name= win_spec->name();
    if (!win_spec_name)
      break;
    if (name && my_strcasecmp(system_charset_info, name, win_spec_name) == 0)
    {
      my_error(ER_DUP_WINDOW_NAME, MYF(0), name);
      return true;
    }
    if (ref_name &&
        my_strcasecmp(system_charset_info, ref_name, win_spec_name) == 0)
    {
      if (partition_list->elements)
      {
        my_error(ER_PARTITION_LIST_IN_REFERENCING_WINDOW_SPEC, MYF(0),
                 ref_name);
        return true;
      }
      if (win_spec->order_list->elements && order_list->elements)
      {
        my_error(ER_ORDER_LIST_IN_REFERENCING_WINDOW_SPEC, MYF(0), ref_name);
        return true;
      } 
      if (win_spec->window_frame)
      {
        my_error(ER_WINDOW_FRAME_IN_REFERENCED_WINDOW_SPEC, MYF(0), ref_name);
        return true;
      }
      referenced_win_spec= win_spec;
      if (partition_list->elements == 0)
        partition_list= win_spec->partition_list;
      if (order_list->elements == 0)
        order_list= win_spec->order_list;
    }
  }
  if (ref_name && !referenced_win_spec)
  {
    my_error(ER_WRONG_WINDOW_SPEC_NAME, MYF(0), ref_name);
    return true;
  }
  window_names_are_checked= true;
  return false;
}

void
Window_spec::print(String *str, enum_query_type query_type)
{
  str->append('(');
  if (partition_list->first)
  {
    str->append(STRING_WITH_LEN(" partition by "));
    st_select_lex::print_order(str, partition_list->first, query_type);
  }
  if (order_list->first)
  {
    str->append(STRING_WITH_LEN(" order by "));
    st_select_lex::print_order(str, order_list->first, query_type);
  }
  if (window_frame)
    window_frame->print(str, query_type);
  str->append(')');
}

bool
Window_frame::check_frame_bounds()
{
  if ((top_bound->is_unbounded() &&
       top_bound->precedence_type == Window_frame_bound::FOLLOWING) ||
      (bottom_bound->is_unbounded() &&
       bottom_bound->precedence_type == Window_frame_bound::PRECEDING) ||
      (top_bound->precedence_type == Window_frame_bound::CURRENT &&
       bottom_bound->precedence_type == Window_frame_bound::PRECEDING) ||
      (bottom_bound->precedence_type == Window_frame_bound::CURRENT &&
       top_bound->precedence_type == Window_frame_bound::FOLLOWING))
  {
    my_error(ER_BAD_COMBINATION_OF_WINDOW_FRAME_BOUND_SPECS, MYF(0));
    return true;
  }

  return false;
}


void
Window_frame::print(String *str, enum_query_type query_type)
{
  switch (units) {
  case UNITS_ROWS:
    str->append(STRING_WITH_LEN(" rows "));
    break;
  case UNITS_RANGE:
    str->append(STRING_WITH_LEN(" range "));
    break; 
  default:
    DBUG_ASSERT(0);
  }

  str->append(STRING_WITH_LEN("between "));
  top_bound->print(str, query_type);
  str->append(STRING_WITH_LEN(" and "));
  bottom_bound->print(str, query_type);
 
  if (exclusion != EXCL_NONE)
  {
     str->append(STRING_WITH_LEN(" exclude ")); 
     switch (exclusion) {
     case EXCL_CURRENT_ROW: 
       str->append(STRING_WITH_LEN(" current row "));
       break;
     case EXCL_GROUP: 
       str->append(STRING_WITH_LEN(" group "));
       break;
     case EXCL_TIES: 
       str->append(STRING_WITH_LEN(" ties "));
       break;
     default: 
       DBUG_ASSERT(0);
       ;
     }
  } 
}


void
Window_frame_bound::print(String *str, enum_query_type query_type)
{
  if (precedence_type == CURRENT)
  {
    str->append(STRING_WITH_LEN(" current row "));
    return;
  }
  if (is_unbounded())
    str->append(STRING_WITH_LEN(" unbounded "));
  else
    offset->print(str ,query_type);  
  switch (precedence_type) {
  case PRECEDING:
    str->append(STRING_WITH_LEN(" preceding "));
    break;
  case FOLLOWING:
    str->append(STRING_WITH_LEN(" following "));
    break;
  default:
    DBUG_ASSERT(0);
  }
}

/*
  Setup window functions in a select
*/

int
setup_windows(THD *thd, Ref_ptr_array ref_pointer_array, TABLE_LIST *tables,
              List<Item> &fields, List<Item> &all_fields,
              List<Window_spec> &win_specs, List<Item_window_func> &win_funcs)
{
  Window_spec *win_spec;
  DBUG_ENTER("setup_windows");
  List_iterator<Window_spec> it(win_specs);

  /* 
    Move all unnamed specifications after the named ones.
    We could have avoided it if we had built two separate lists for
    named and unnamed specifications.
  */
  Query_arena *arena, backup;
  arena= thd->activate_stmt_arena_if_needed(&backup);
  uint i = 0;
  uint elems= win_specs.elements;
  while ((win_spec= it++) && i++ < elems)
  {
    if (win_spec->name() == NULL)
    {
      it.remove();
      win_specs.push_back(win_spec);
    }
  }
  if (arena)
    thd->restore_active_arena(arena, &backup);

  it.rewind();

  List_iterator_fast<Window_spec> itp(win_specs);

  while ((win_spec= it++))
  {
    bool hidden_group_fields;
    if (win_spec->check_window_names(itp) ||
        setup_group(thd, ref_pointer_array, tables, fields, all_fields,
                    win_spec->partition_list->first, &hidden_group_fields,
                    true) ||
        setup_order(thd, ref_pointer_array, tables, fields, all_fields,
                    win_spec->order_list->first, true) ||
        (win_spec->window_frame && 
         win_spec->window_frame->check_frame_bounds()))
    {
      DBUG_RETURN(1);
    }

    if (win_spec->window_frame &&
        win_spec->window_frame->exclusion != Window_frame::EXCL_NONE)
    {
      my_error(ER_FRAME_EXCLUSION_NOT_SUPPORTED, MYF(0));
      DBUG_RETURN(1);
    }
    /*
       For  "win_func() OVER (ORDER BY order_list RANGE BETWEEN ...)",
       - ORDER BY order_list must not be ommitted
       - the list must have a single element.
    */
    if (win_spec->window_frame && 
        win_spec->window_frame->units == Window_frame::UNITS_RANGE)
    {
      if (win_spec->order_list->elements != 1)
      {
        my_error(ER_RANGE_FRAME_NEEDS_SIMPLE_ORDERBY, MYF(0));
        DBUG_RETURN(1);
      }

      /*
        "The declared type of SK shall be numeric, datetime, or interval"
        we don't support datetime or interval, yet.
      */
      Item_result rtype= win_spec->order_list->first->item[0]->result_type();
      if (rtype != REAL_RESULT && rtype != INT_RESULT && 
          rtype != DECIMAL_RESULT)
      {
        my_error(ER_WRONG_TYPE_FOR_RANGE_FRAME, MYF(0));
        DBUG_RETURN(1);
      }

      /*
        "The declared type of UVS shall be numeric if the declared type of SK 
        is numeric; otherwise, it shall be an interval type that may be added
        to or subtracted from the declared type of SK"
      */
      Window_frame_bound *bounds[]= {win_spec->window_frame->top_bound,
                                     win_spec->window_frame->bottom_bound,
                                     NULL};
      for (Window_frame_bound **pbound= &bounds[0]; *pbound; pbound++)
      {
        if (!(*pbound)->is_unbounded() &&
            ((*pbound)->precedence_type == Window_frame_bound::FOLLOWING ||
             (*pbound)->precedence_type == Window_frame_bound::PRECEDING))
        {
          Item_result rtype= (*pbound)->offset->result_type();
          if (rtype != REAL_RESULT && rtype != INT_RESULT && 
              rtype != DECIMAL_RESULT)
          {
            my_error(ER_WRONG_TYPE_FOR_RANGE_FRAME, MYF(0));
            DBUG_RETURN(1);
          }
        }
      }
    }

    /* "ROWS PRECEDING|FOLLOWING $n" must have a numeric $n */
    if (win_spec->window_frame && 
        win_spec->window_frame->units == Window_frame::UNITS_ROWS)
    {
      Window_frame_bound *bounds[]= {win_spec->window_frame->top_bound,
                                     win_spec->window_frame->bottom_bound,
                                     NULL};
      for (Window_frame_bound **pbound= &bounds[0]; *pbound; pbound++)
      {
        if (!(*pbound)->is_unbounded() &&
            ((*pbound)->precedence_type == Window_frame_bound::FOLLOWING ||
             (*pbound)->precedence_type == Window_frame_bound::PRECEDING))
        {
          Item *offset= (*pbound)->offset;
          if (offset->result_type() != INT_RESULT)
          {
            my_error(ER_WRONG_TYPE_FOR_ROWS_FRAME, MYF(0));
            DBUG_RETURN(1);
          }
        }
      }
    }
  }

  DBUG_RETURN(0);
}

/////////////////////////////////////////////////////////////////////////////
// Sorting window functions to minimize the number of table scans
// performed during the computation of these functions 
/////////////////////////////////////////////////////////////////////////////

#define CMP_LT        -2    // Less than
#define CMP_LT_C      -1    // Less than and compatible
#define CMP_EQ         0    // Equal to
#define CMP_GT_C       1    // Greater than and compatible
#define CMP_GT         2    // Greater then

static
int compare_order_elements(ORDER *ord1, int weight1,
                           ORDER *ord2, int weight2)
{
  if (*ord1->item == *ord2->item && ord1->direction == ord2->direction)
    return CMP_EQ;
  Item *item1= (*ord1->item)->real_item();
  Item *item2= (*ord2->item)->real_item();

  bool item1_field= (item1->type() == Item::FIELD_ITEM);
  bool item2_field= (item2->type() == Item::FIELD_ITEM);

  ptrdiff_t cmp;
  if (item1_field && item2_field)
  {
    DBUG_ASSERT(((Item_field *) item1)->field->table ==
                ((Item_field *) item2)->field->table);
    cmp= ((Item_field *) item1)->field->field_index -
         ((Item_field *) item2)->field->field_index;
  }
  else if (item1_field && !item2_field)
    return CMP_LT;
  else if (!item1_field && item2_field)
    return CMP_LT;
  else
  {
    /*
      Ok, item1_field==NULL and item2_field==NULL.
      We're not able to compare Item expressions. Order them according to
      their passed "weight" (which comes from Window_spec::win_spec_number):
    */
    if (weight1 != weight2)
      cmp= weight1 - weight2;
    else
    {
      /*
        The weight is the same. That is, the elements come from the same
        window specification... This shouldn't happen.
      */
      DBUG_ASSERT(0);
      cmp= item1 - item2;
    }
  }

  if (cmp == 0)
  {
    if (ord1->direction == ord2->direction)
      return CMP_EQ;
    return ord1->direction > ord2->direction ? CMP_GT : CMP_LT;
  }
  else
    return cmp > 0 ? CMP_GT : CMP_LT;
}

static
int compare_order_lists(SQL_I_List<ORDER> *part_list1,
                        int spec_number1,
                        SQL_I_List<ORDER> *part_list2,
                        int spec_number2)
{
  if (part_list1 == part_list2)
    return CMP_EQ;
  ORDER *elem1= part_list1->first;
  ORDER *elem2= part_list2->first;
  for ( ; elem1 && elem2; elem1= elem1->next, elem2= elem2->next)
  {
    int cmp;
    // remove all constants as we don't need them for comparision
    while(elem1 && ((*elem1->item)->real_item())->const_item())
    {
      elem1= elem1->next;
      continue;
    }

    while(elem2 && ((*elem2->item)->real_item())->const_item())
    {
      elem2= elem2->next;
      continue;
    }

    if (!elem1 || !elem2)
      break;

    if ((cmp= compare_order_elements(elem1, spec_number1,
                                     elem2, spec_number2)))
      return cmp;
  }
  if (elem1)
    return CMP_GT_C;
  if (elem2)
    return CMP_LT_C;
  return CMP_EQ;
}


static
int compare_window_frame_bounds(Window_frame_bound *win_frame_bound1,
                                Window_frame_bound *win_frame_bound2,
                                bool is_bottom_bound)
{ 
  int res;
  if (win_frame_bound1->precedence_type != win_frame_bound2->precedence_type)
  {
    res= win_frame_bound1->precedence_type > win_frame_bound2->precedence_type ?
         CMP_GT : CMP_LT;
    if (is_bottom_bound)
      res= -res;
    return res;
  }

  if (win_frame_bound1->is_unbounded() && win_frame_bound2->is_unbounded())
    return CMP_EQ;

  if (!win_frame_bound1->is_unbounded() && !win_frame_bound2->is_unbounded())
  { 
    if (win_frame_bound1->offset->eq(win_frame_bound2->offset, true))
      return CMP_EQ;
    else
    {
      res= strcmp(win_frame_bound1->offset->name,
                  win_frame_bound2->offset->name);
      res= res > 0 ? CMP_GT : CMP_LT;
      if (is_bottom_bound)
        res= -res;
      return res;
    }
  }

  /* 
    Here we have:
    win_frame_bound1->is_unbounded() != win_frame_bound1->is_unbounded()
  */
  return is_bottom_bound != win_frame_bound1->is_unbounded() ? CMP_LT : CMP_GT; 
}


static
int compare_window_frames(Window_frame *win_frame1,
                          Window_frame *win_frame2)
{
  int cmp;

  if (win_frame1 == win_frame2)
    return CMP_EQ;

  if (!win_frame1)
    return CMP_LT;

  if (!win_frame2)
    return CMP_GT;

  if (win_frame1->units != win_frame2->units)
    return win_frame1->units > win_frame2->units ? CMP_GT : CMP_LT;

  cmp= compare_window_frame_bounds(win_frame1->top_bound,
                                   win_frame2->top_bound,
                                   false);
  if (cmp)
    return cmp;

  cmp= compare_window_frame_bounds(win_frame1->bottom_bound,
                                   win_frame2->bottom_bound,
                                   true);  
  if (cmp)
    return cmp;

  if (win_frame1->exclusion != win_frame2->exclusion)
    return win_frame1->exclusion > win_frame2->exclusion ? CMP_GT_C : CMP_LT_C;

  return CMP_EQ;
}

static 
int compare_window_spec_joined_lists(Window_spec *win_spec1,
                                     Window_spec *win_spec2)
{
  win_spec1->join_partition_and_order_lists();
  win_spec2->join_partition_and_order_lists();
  int cmp= compare_order_lists(win_spec1->partition_list, 
                               win_spec1->win_spec_number,
                               win_spec2->partition_list,
                               win_spec2->win_spec_number);
  win_spec1->disjoin_partition_and_order_lists();
  win_spec2->disjoin_partition_and_order_lists();
  return cmp;
}


static
int compare_window_funcs_by_window_specs(Item_window_func *win_func1,
                                         Item_window_func *win_func2,
                                         void *arg)
{
  int cmp;
  Window_spec *win_spec1= win_func1->window_spec;
  Window_spec *win_spec2= win_func2->window_spec;
  if (win_spec1 == win_spec2)
    return CMP_EQ;
  cmp= compare_order_lists(win_spec1->partition_list, 
                           win_spec1->win_spec_number,
                           win_spec2->partition_list,
                           win_spec2->win_spec_number);
  if (cmp == CMP_EQ)
  {
    /* 
      Partition lists contain the same elements. 
      Let's use only one of the lists.
    */
    if (!win_spec1->name() && win_spec2->name())
    {
      win_spec1->save_partition_list= win_spec1->partition_list;
      win_spec1->partition_list= win_spec2->partition_list;
    }
    else
    {
      win_spec2->save_partition_list= win_spec2->partition_list;
      win_spec2->partition_list= win_spec1->partition_list;
    }

    cmp= compare_order_lists(win_spec1->order_list,
                             win_spec1->win_spec_number,
                             win_spec2->order_list,
                             win_spec2->win_spec_number);

    if (cmp != CMP_EQ)
      return cmp;

    /* 
       Order lists contain the same elements.
       Let's use only one of the lists.
    */
    if (!win_spec1->name() && win_spec2->name())
    {
      win_spec1->save_order_list= win_spec2->order_list;
      win_spec1->order_list= win_spec2->order_list;
    }
    else
    {
      win_spec1->save_order_list= win_spec2->order_list;
      win_spec2->order_list= win_spec1->order_list;
    }

    cmp= compare_window_frames(win_spec1->window_frame,
                               win_spec2->window_frame);

    if (cmp != CMP_EQ)
      return cmp;

    /* Window frames are equal. Let's use only one of them. */
    if (!win_spec1->name() && win_spec2->name())
      win_spec1->window_frame= win_spec2->window_frame;
    else
      win_spec2->window_frame= win_spec1->window_frame;

    return CMP_EQ;
  }
  
  if (cmp == CMP_GT || cmp == CMP_LT)
    return cmp;

  /* one of the partitions lists is the proper beginning of the another */
  cmp= compare_window_spec_joined_lists(win_spec1, win_spec2);

  if (CMP_LT_C <= cmp && cmp <= CMP_GT_C) 
    cmp= win_spec1->partition_list->elements <
      win_spec2->partition_list->elements ? CMP_GT_C : CMP_LT_C;

  return cmp;
}


#define  SORTORDER_CHANGE_FLAG    1
#define  PARTITION_CHANGE_FLAG    2
#define  FRAME_CHANGE_FLAG        4

typedef int (*Item_window_func_cmp)(Item_window_func *f1,
                                    Item_window_func *f2,
                                    void *arg); 
/*
  @brief
    Sort window functions so that those that can be computed together are
    adjacent.

  @detail
    Sort window functions by their
     - required sorting order,
     - partition list,
     - window frame compatibility.

    The changes between the groups are marked by setting item_window_func->marker.
*/

static
void order_window_funcs_by_window_specs(List<Item_window_func> *win_func_list)
{
  if (win_func_list->elements == 0)
    return;

  bubble_sort<Item_window_func>(win_func_list,
                                compare_window_funcs_by_window_specs,
                                NULL);

  List_iterator_fast<Item_window_func> it(*win_func_list);
  Item_window_func *prev= it++;
  prev->marker= SORTORDER_CHANGE_FLAG |
                PARTITION_CHANGE_FLAG |
                FRAME_CHANGE_FLAG;
  Item_window_func *curr;
  while ((curr= it++))
  {
    Window_spec *win_spec_prev= prev->window_spec;
    Window_spec *win_spec_curr= curr->window_spec;
    curr->marker= 0;
    if (!(win_spec_prev->partition_list == win_spec_curr->partition_list &&
          win_spec_prev->order_list == win_spec_curr->order_list))
    {
      int cmp;
      if (win_spec_prev->partition_list == win_spec_curr->partition_list)
        cmp= compare_order_lists(win_spec_prev->order_list,
                                 win_spec_prev->win_spec_number,
                                 win_spec_curr->order_list,
                                 win_spec_curr->win_spec_number);
      else
        cmp= compare_window_spec_joined_lists(win_spec_prev, win_spec_curr);
      if (!(CMP_LT_C <= cmp && cmp <= CMP_GT_C))
      {
        curr->marker= SORTORDER_CHANGE_FLAG |
                      PARTITION_CHANGE_FLAG |
                      FRAME_CHANGE_FLAG;
      }
      else if (win_spec_prev->partition_list != win_spec_curr->partition_list)
      {
        curr->marker|= PARTITION_CHANGE_FLAG | FRAME_CHANGE_FLAG;
      }
    }
    else if (win_spec_prev->window_frame != win_spec_curr->window_frame)
      curr->marker|= FRAME_CHANGE_FLAG;

    prev= curr;
  }
}


/////////////////////////////////////////////////////////////////////////////


/////////////////////////////////////////////////////////////////////////////
// Window Frames support 
/////////////////////////////////////////////////////////////////////////////

// note: make rr_from_pointers static again when not need it here anymore
int rr_from_pointers(READ_RECORD *info);


/////////////////////////////////////////////////////////////////////////////


/*
  A cursor over a sequence of rowids. One can
   - Move to next rowid
   - jump to given number in the sequence
   - Know the number of the current rowid (i.e. how many rowids have been read)
*/

class Rowid_seq_cursor
{
public:
  Rowid_seq_cursor() : io_cache(NULL), ref_buffer(0) {}
  virtual ~Rowid_seq_cursor()
  {
    if (ref_buffer)
      my_free(ref_buffer);
    if (io_cache)
    {
      end_slave_io_cache(io_cache);
      my_free(io_cache);
      io_cache= NULL;
    }
  }

private:
  /* Length of one rowid element */
  size_t ref_length;

  /* If io_cache=!NULL, use it */
  IO_CACHE *io_cache;
  uchar *ref_buffer;   /* Buffer for the last returned rowid */
  ha_rows rownum;     /* Number of the rowid that is about to be returned */
  ha_rows current_ref_buffer_rownum;
  bool ref_buffer_valid;

  /* The following are used when we are reading from an array of pointers */
  uchar *cache_start;
  uchar *cache_pos;
  uchar *cache_end;
public:

  void init(READ_RECORD *info)
  {
    ref_length= info->ref_length;
    if (info->read_record == rr_from_pointers)
    {
      io_cache= NULL;
      cache_start= info->cache_pos;
      cache_pos=   info->cache_pos;
      cache_end=   info->cache_end;
    }
    else
    {
      //DBUG_ASSERT(info->read_record == rr_from_tempfile);
      rownum= 0;
      io_cache= (IO_CACHE*)my_malloc(sizeof(IO_CACHE), MYF(0));
      init_slave_io_cache(info->io_cache, io_cache);

      ref_buffer= (uchar*)my_malloc(ref_length, MYF(0));
      ref_buffer_valid= false;
    }
  }

  virtual int next()
  {
    /* Allow multiple next() calls in EOF state. */
    if (at_eof())
        return -1;

    if (io_cache)
    {
      rownum++;
    }
    else
    {
      cache_pos+= ref_length;
      DBUG_ASSERT(cache_pos <= cache_end);
    }
    return 0;
  }

  virtual int prev()
  {
    if (io_cache)
    {
      if (rownum == 0)
        return -1;

      rownum--;
      return 0;
    }
    else
    {
      /* Allow multiple prev() calls when positioned at the start. */
      if (cache_pos == cache_start)
        return -1;
      cache_pos-= ref_length;
      DBUG_ASSERT(cache_pos >= cache_start);
      return 0;
    }
  }

  ha_rows get_rownum() const
  {
    if (io_cache)
      return rownum;
    else
      return (cache_pos - cache_start) / ref_length;
  }

  void move_to(ha_rows row_number)
  {
    if (io_cache)
    {
      rownum= row_number;
    }
    else
    {
      cache_pos= MY_MIN(cache_end, cache_start + row_number * ref_length);
      DBUG_ASSERT(cache_pos <= cache_end);
    }
  }

protected:
  bool at_eof()
  {
    if (io_cache)
    {
      return rownum * ref_length >= io_cache->end_of_file;
    }
    else
      return (cache_pos == cache_end);
  }

  bool get_curr_rowid(uchar **row_id)
  {
    if (io_cache)
    {
      DBUG_ASSERT(!at_eof());
      if (!ref_buffer_valid || current_ref_buffer_rownum != rownum)
      {
        seek_io_cache(io_cache, rownum * ref_length);
        if (my_b_read(io_cache,ref_buffer,ref_length))
        {
          /* Error reading from file. */
          return true;
        }
        ref_buffer_valid= true;
        current_ref_buffer_rownum = rownum;
      }
      *row_id = ref_buffer;
      return false;
    }
    else
    {
      *row_id= cache_pos;
      return false;
    }
  }
};


/*
  Cursor which reads from rowid sequence and also retrieves table rows.
*/

class Table_read_cursor : public Rowid_seq_cursor
{
public:
  virtual ~Table_read_cursor() {}

  void init(READ_RECORD *info)
  {
    Rowid_seq_cursor::init(info);
    table= info->table;
    record= info->record;
  }

  virtual int fetch()
  {
    if (at_eof())
      return -1;

    uchar* curr_rowid;
    if (get_curr_rowid(&curr_rowid))
      return -1;
    return table->file->ha_rnd_pos(record, curr_rowid);
  }

private:
  /* The table that is acccesed by this cursor. */
  TABLE *table;
  /* Buffer where to store the table's record data. */
  uchar *record;

  // TODO(spetrunia): should move_to() also read row here?
};


/*
  A cursor which only moves within a partition. The scan stops at the partition
  end, and it needs an explicit command to move to the next partition.

  This cursor can not move backwards.
*/

class Partition_read_cursor : public Table_read_cursor
{
public:
  Partition_read_cursor(THD *thd, SQL_I_List<ORDER> *partition_list) :
    bound_tracker(thd, partition_list) {}

  void init(READ_RECORD *info)
  {
    Table_read_cursor::init(info);
    bound_tracker.init();
    end_of_partition= false;
  }

  /*
    Informs the cursor that we need to move into the next partition.
    The next partition is provided in two ways:
    - in table->record[0]..
    - rownum parameter has the row number.
  */
  void on_next_partition(ha_rows rownum)
  {
    /* Remember the sort key value from the new partition */
    move_to(rownum);
    bound_tracker.check_if_next_group();
    end_of_partition= false;

  }

  /*
    This returns -1 when end of partition was reached.
  */
  int next()
  {
    int res;
    if (end_of_partition)
      return -1;

    if ((res= Table_read_cursor::next()) ||
        (res= fetch()))
    {
      /* TODO(cvicentiu) This does not consider table read failures.
         Perhaps assuming end of table like this is fine in that case. */

      /* This row is the final row in the table. To maintain semantics
         that cursors always point to the last valid row, move back one step,
         but mark end_of_partition as true. */
      Table_read_cursor::prev();
      end_of_partition= true;
      return res;
    }

    if (bound_tracker.compare_with_cache())
    {
      /* This row is part of a new partition, don't move
         forward any more untill we get informed of a new partition. */
      Table_read_cursor::prev();
      end_of_partition= true;
      return -1;
    }
    return 0;
  }

private:
  Group_bound_tracker bound_tracker;
  bool end_of_partition;
};

/////////////////////////////////////////////////////////////////////////////

/*
  Window frame bound cursor. Abstract interface.

  @detail
    The cursor moves within the partition that the current row is in.
    It may be ahead or behind the current row.

    The cursor also assumes that the current row moves forward through the
    partition and will move to the next adjacent partition after this one.

    List of all cursor classes:
      Frame_cursor
        Frame_range_n_top
        Frame_range_n_bottom

        Frame_range_current_row_top
        Frame_range_current_row_bottom

        Frame_n_rows_preceding
        Frame_n_rows_following

        Frame_rows_current_row_top = Frame_n_rows_preceding(0)
        Frame_rows_current_row_bottom

        // These handle both RANGE and ROWS-type bounds
        Frame_unbounded_preceding
        Frame_unbounded_following

        // This is not used as a frame bound, it counts rows in the partition:
        Frame_unbounded_following_set_count : public Frame_unbounded_following

  @todo
  - if we want to allocate this on the MEM_ROOT we should make sure 
    it is not re-allocated for every subquery execution.
*/

class Frame_cursor : public Sql_alloc
{
public:
  Frame_cursor() : sum_functions(), perform_no_action(false) {}

  virtual void init(READ_RECORD *info) {};

  bool add_sum_func(Item_sum* item)
  {
    return sum_functions.push_back(item);
  }
  /*
    Current row has moved to the next partition and is positioned on the first
    row there. Position the frame bound accordingly.

    @param first   -  TRUE means this is the first partition
    @param item    -  Put or remove rows from there.

    @detail
      - if first==false, the caller guarantees that tbl->record[0] points at the
        first row in the new partition.
      - if first==true, we are just starting in the first partition and no such
        guarantee is provided.

      - The callee may move tbl->file and tbl->record[0] to point to some other
        row.
  */
  virtual void pre_next_partition(ha_rows rownum) {};
  virtual void next_partition(ha_rows rownum)=0;

  /*
    The current row has moved one row forward.
    Move this frame bound accordingly, and update the value of aggregate
    function as necessary.
  */
  virtual void pre_next_row() {};
  virtual void next_row()=0;

  virtual bool is_outside_computation_bounds() const { return false; };

  virtual ~Frame_cursor() {}

  /*
     Regular frame cursors add or remove values from the sum functions they
     manage. By calling this method, they will only perform the required
     movement within the table, but no adding/removing will happen.
  */
  void set_no_action()
  {
    perform_no_action= true;
  }

  /* Retrieves the row number that this cursor currently points at. */
  virtual ha_rows get_curr_rownum() const= 0;

protected:
  inline void add_value_to_items()
  {
    if (perform_no_action)
      return;

    List_iterator_fast<Item_sum> it(sum_functions);
    Item_sum *item_sum;
    while ((item_sum= it++))
    {
      item_sum->add();
    }
  }

  inline void remove_value_from_items()
  {
    if (perform_no_action)
      return;

    List_iterator_fast<Item_sum> it(sum_functions);
    Item_sum *item_sum;
    while ((item_sum= it++))
    {
      item_sum->remove();
    }
  }

  /* Clear all sum functions handled by this cursor. */
  void clear_sum_functions()
  {
    List_iterator_fast<Item_sum> iter_sum_func(sum_functions);
    Item_sum *sum_func;
    while ((sum_func= iter_sum_func++))
    {
      sum_func->clear();
    }
  }

  /* Sum functions that this cursor handles. */
  List<Item_sum> sum_functions;

private:
  bool perform_no_action;
};

/*
  A class that owns cursor objects associated with a specific window function.
*/
class Cursor_manager
{
public:
  bool add_cursor(Frame_cursor *cursor)
  {
    return cursors.push_back(cursor);
  }

  void initialize_cursors(READ_RECORD *info)
  {
    List_iterator_fast<Frame_cursor> iter(cursors);
    Frame_cursor *fc;
    while ((fc= iter++))
      fc->init(info);
  }

  void notify_cursors_partition_changed(ha_rows rownum)
  {
    List_iterator_fast<Frame_cursor> iter(cursors);
    Frame_cursor *cursor;
    while ((cursor= iter++))
      cursor->pre_next_partition(rownum);

    iter.rewind();
    while ((cursor= iter++))
      cursor->next_partition(rownum);
  }

  void notify_cursors_next_row()
  {
    List_iterator_fast<Frame_cursor> iter(cursors);
    Frame_cursor *cursor;
    while ((cursor= iter++))
      cursor->pre_next_row();

    iter.rewind();
    while ((cursor= iter++))
      cursor->next_row();
  }

  ~Cursor_manager() { cursors.delete_elements(); }

private:
  /* List of the cursors that this manager owns. */
  List<Frame_cursor> cursors;
};



//////////////////////////////////////////////////////////////////////////////
// RANGE-type frames
//////////////////////////////////////////////////////////////////////////////

/*
  Frame_range_n_top handles the top end of RANGE-type frame.

  That is, it handles:
    RANGE BETWEEN n PRECEDING AND ...
    RANGE BETWEEN n FOLLOWING AND ...

  Top of the frame doesn't need to check for partition end, since bottom will
  reach it before.
*/

class Frame_range_n_top : public Frame_cursor
{
  Partition_read_cursor cursor;

  Cached_item_item *range_expr;

  Item *n_val;
  Item *item_add;

  const bool is_preceding;

  bool end_of_partition;

  /*
     1  when order_list uses ASC ordering
    -1  when order_list uses DESC ordering
  */
  int order_direction;
public:
  Frame_range_n_top(THD *thd,
                    SQL_I_List<ORDER> *partition_list,
                    SQL_I_List<ORDER> *order_list,
                    bool is_preceding_arg, Item *n_val_arg) :
    cursor(thd, partition_list), n_val(n_val_arg), item_add(NULL),
    is_preceding(is_preceding_arg)
  {
    DBUG_ASSERT(order_list->elements == 1);
    Item *src_expr= order_list->first->item[0];
    if (order_list->first->direction == ORDER::ORDER_ASC)
      order_direction= 1;
    else
      order_direction= -1;

    range_expr= (Cached_item_item*) new_Cached_item(thd, src_expr, FALSE);

    bool use_minus= is_preceding;
    if (order_direction == -1)
      use_minus= !use_minus;

    if (use_minus)
      item_add= new (thd->mem_root) Item_func_minus(thd, src_expr, n_val);
    else
      item_add= new (thd->mem_root) Item_func_plus(thd, src_expr, n_val);

    item_add->fix_fields(thd, &item_add);
  }

  void init(READ_RECORD *info)
  {
    cursor.init(info);
  }

  void pre_next_partition(ha_rows rownum)
  {
    // Save the value of FUNC(current_row)
    range_expr->fetch_value_from(item_add);

    cursor.on_next_partition(rownum);
    end_of_partition= false;
  }

  void next_partition(ha_rows rownum)
  {
    walk_till_non_peer();
  }

  void pre_next_row()
  {
    if (end_of_partition)
      return;
    range_expr->fetch_value_from(item_add);
  }

  void next_row()
  {
    if (end_of_partition)
      return;
    /*
      Ok, our cursor is at the first row R where
        (prev_row + n) >= R
      We need to check about the current row.
    */
    walk_till_non_peer();
  }

  ha_rows get_curr_rownum() const
  {
    return cursor.get_rownum();
  }

  bool is_outside_computation_bounds() const
  {
    if (end_of_partition)
      return true;
    return false;
  }

private:
  void walk_till_non_peer()
  {
    if (cursor.fetch()) // ERROR
      return;
    // Current row is not a peer.
    if (order_direction * range_expr->cmp_read_only() <= 0)
      return;
    remove_value_from_items();

    int res;
    while (!(res= cursor.next()))
    {
      /* Note, no need to fetch the value explicitly here. The partition
         read cursor will fetch it to check if the partition has changed.
         TODO(cvicentiu) make this piece of information not necessary by
         reimplementing Partition_read_cursor.
      */
      if (order_direction * range_expr->cmp_read_only() <= 0)
        break;
      remove_value_from_items();
    }
    if (res)
      end_of_partition= true;
  }

};


/*
  Frame_range_n_bottom handles bottom end of RANGE-type frame.

  That is, it handles frame bounds in form:
    RANGE BETWEEN ... AND n PRECEDING
    RANGE BETWEEN ... AND n FOLLOWING

  Bottom end moves first so it needs to check for partition end
  (todo: unless it's PRECEDING and in that case it doesnt)
  (todo: factor out common parts with Frame_range_n_top into
   a common ancestor)
*/

class Frame_range_n_bottom: public Frame_cursor
{
  Partition_read_cursor cursor;

  Cached_item_item *range_expr;

  Item *n_val;
  Item *item_add;

  const bool is_preceding;

  bool end_of_partition;

  /*
     1  when order_list uses ASC ordering
    -1  when order_list uses DESC ordering
  */
  int order_direction;
public:
  Frame_range_n_bottom(THD *thd,
                       SQL_I_List<ORDER> *partition_list,
                       SQL_I_List<ORDER> *order_list,
                       bool is_preceding_arg, Item *n_val_arg) :
    cursor(thd, partition_list), n_val(n_val_arg), item_add(NULL),
    is_preceding(is_preceding_arg), added_values(false)
  {
    DBUG_ASSERT(order_list->elements == 1);
    Item *src_expr= order_list->first->item[0];

    if (order_list->first->direction == ORDER::ORDER_ASC)
      order_direction= 1;
    else
      order_direction= -1;

    range_expr= (Cached_item_item*) new_Cached_item(thd, src_expr, FALSE);

    bool use_minus= is_preceding;
    if (order_direction == -1)
      use_minus= !use_minus;

    if (use_minus)
      item_add= new (thd->mem_root) Item_func_minus(thd, src_expr, n_val);
    else
      item_add= new (thd->mem_root) Item_func_plus(thd, src_expr, n_val);

    item_add->fix_fields(thd, &item_add);
  }

  void init(READ_RECORD *info)
  {
    cursor.init(info);
  }

  void pre_next_partition(ha_rows rownum)
  {
    // Save the value of FUNC(current_row)
    range_expr->fetch_value_from(item_add);

    cursor.on_next_partition(rownum);
    end_of_partition= false;
    added_values= false;
  }

  void next_partition(ha_rows rownum)
  {
    cursor.move_to(rownum);
    walk_till_non_peer();
  }

  void pre_next_row()
  {
    if (end_of_partition)
      return;
    range_expr->fetch_value_from(item_add);
  }

  void next_row()
  {
    if (end_of_partition)
      return;
    /*
      Ok, our cursor is at the first row R where
        (prev_row + n) >= R
      We need to check about the current row.
    */
    walk_till_non_peer();
  }

  bool is_outside_computation_bounds() const
  {
    if (!added_values)
      return true;
    return false;
  }

  ha_rows get_curr_rownum() const
  {
    if (end_of_partition)
      return cursor.get_rownum(); // Cursor does not pass over partition bound.
    else
      return cursor.get_rownum() - 1; // Cursor is placed on first non peer.
  }

private:
  bool added_values;

  void walk_till_non_peer()
  {
    cursor.fetch();
    // Current row is not a peer.
    if (order_direction * range_expr->cmp_read_only() < 0)
      return;

    add_value_to_items(); // Add current row.
    added_values= true;
    int res;
    while (!(res= cursor.next()))
    {
      if (order_direction * range_expr->cmp_read_only() < 0)
        break;
      add_value_to_items();
    }
    if (res)
      end_of_partition= true;
  }
};


/*
  RANGE BETWEEN ... AND CURRENT ROW, bottom frame bound for CURRENT ROW
     ...
   | peer1
   | peer2  <----- current_row
   | peer3
   +-peer4  <----- the cursor points here. peer4 itself is included.
     nonpeer1
     nonpeer2

  This bound moves in front of the current_row. It should be a the first row
  that is still a peer of the current row.
*/

class Frame_range_current_row_bottom: public Frame_cursor
{
  Partition_read_cursor cursor;

  Group_bound_tracker peer_tracker;

  bool dont_move;
public:
  Frame_range_current_row_bottom(THD *thd,
                                 SQL_I_List<ORDER> *partition_list,
                                 SQL_I_List<ORDER> *order_list) :
    cursor(thd, partition_list), peer_tracker(thd, order_list)
  {
  }

  void init(READ_RECORD *info)
  {
    cursor.init(info);
    peer_tracker.init();
  }

  void pre_next_partition(ha_rows rownum)
  {
    // Save the value of the current_row
    peer_tracker.check_if_next_group();
    cursor.on_next_partition(rownum);
    // Add the current row now because our cursor has already seen it
    add_value_to_items();
  }

  void next_partition(ha_rows rownum)
  {
    walk_till_non_peer();
  }

  void pre_next_row()
  {
    dont_move= !peer_tracker.check_if_next_group();
  }

  void next_row()
  {
    // Check if our cursor is pointing at a peer of the current row.
    // If not, move forward until that becomes true
    if (dont_move)
    {
      /*
        Our current is not a peer of the current row.
        No need to move the bound.
      */
      return;
    }
    walk_till_non_peer();
  }

  ha_rows get_curr_rownum() const
  {
    return cursor.get_rownum();
  }

private:
  void walk_till_non_peer()
  {
    /*
      Walk forward until we've met first row that's not a peer of the current
      row
    */
    while (!cursor.next())
    {
      if (peer_tracker.compare_with_cache())
      {
        cursor.prev(); // Move to our peer.
        break;
      }

      add_value_to_items();
    }
  }
};


/*
  RANGE BETWEEN CURRENT ROW AND .... Top CURRENT ROW, RANGE-type frame bound

      nonpeer1
      nonpeer2
    +-peer1  <----- the cursor points here. peer1 itself is included.
    | peer2  
    | peer3  <----- current_row
    | peer4 
      ... 

  It moves behind the current_row. It is located right after the first peer of
  the current_row.
*/

class Frame_range_current_row_top : public Frame_cursor
{
  Group_bound_tracker bound_tracker;

  Table_read_cursor cursor;
  Group_bound_tracker peer_tracker;

  bool move;
public:
  Frame_range_current_row_top(THD *thd,
                              SQL_I_List<ORDER> *partition_list,
                              SQL_I_List<ORDER> *order_list) :
    bound_tracker(thd, partition_list), cursor(), peer_tracker(thd, order_list),
    move(false)
  {}

  void init(READ_RECORD *info)
  {
    bound_tracker.init();

    cursor.init(info);
    peer_tracker.init();
  }

  void pre_next_partition(ha_rows rownum)
  {
    // Fetch the value from the first row
    peer_tracker.check_if_next_group();
    cursor.move_to(rownum);
  }

  void next_partition(ha_rows rownum) {}

  void pre_next_row()
  {
    // Check if the new current_row is a peer of the row that our cursor is
    // pointing to.
    move= peer_tracker.check_if_next_group();
  }

  void next_row()
  {
    if (move)
    {
      /*
        Our cursor is pointing at the first row that was a peer of the previous
        current row. Or, it was the first row in the partition.
      */
      if (cursor.fetch())
        return;

      // todo: need the following check ?
      if (!peer_tracker.compare_with_cache())
        return;
      remove_value_from_items();

      do
      {
        if (cursor.next() || cursor.fetch())
          return;
        if (!peer_tracker.compare_with_cache())
          return;
        remove_value_from_items();
      }
      while (1);
    }
  }

  ha_rows get_curr_rownum() const
  {
    return cursor.get_rownum();
  }
};


/////////////////////////////////////////////////////////////////////////////
// UNBOUNDED frame bounds (shared between RANGE and ROWS)
/////////////////////////////////////////////////////////////////////////////

/*
  UNBOUNDED PRECEDING frame bound
*/
class Frame_unbounded_preceding : public Frame_cursor
{
public:
  Frame_unbounded_preceding(THD *thd,
                            SQL_I_List<ORDER> *partition_list,
                            SQL_I_List<ORDER> *order_list)
  {}

  void init(READ_RECORD *info) {}

  void next_partition(ha_rows rownum)
  {
    /*
      UNBOUNDED PRECEDING frame end just stays on the first row of the
      partition. We are top of the frame, so we don't need to update the sum
      function.
    */
    curr_rownum= rownum;
  }

  void next_row()
  {
    /* Do nothing, UNBOUNDED PRECEDING frame end doesn't move. */
  }

  ha_rows get_curr_rownum() const
  {
    return curr_rownum;
  }

private:
  ha_rows curr_rownum;
};


/*
  UNBOUNDED FOLLOWING frame bound
*/

class Frame_unbounded_following : public Frame_cursor
{
protected:
  Partition_read_cursor cursor;

public:
  Frame_unbounded_following(THD *thd,
      SQL_I_List<ORDER> *partition_list,
      SQL_I_List<ORDER> *order_list) :
    cursor(thd, partition_list) {}

  void init(READ_RECORD *info)
  {
    cursor.init(info);
  }

  void pre_next_partition(ha_rows rownum)
  {
    cursor.on_next_partition(rownum);
  }

  void next_partition(ha_rows rownum)
  {
    /* Activate the first row */
    cursor.fetch();
    add_value_to_items();

    /* Walk to the end of the partition, updating the SUM function */
    while (!cursor.next())
    {
      add_value_to_items();
    }
  }

  void next_row()
  {
    /* Do nothing, UNBOUNDED FOLLOWING frame end doesn't move */
  }

  ha_rows get_curr_rownum() const
  {
    return cursor.get_rownum();
  }
};


class Frame_unbounded_following_set_count : public Frame_unbounded_following
{
public:
  Frame_unbounded_following_set_count(
      THD *thd,
      SQL_I_List<ORDER> *partition_list, SQL_I_List<ORDER> *order_list) :
    Frame_unbounded_following(thd, partition_list, order_list) {}

  void next_partition(ha_rows rownum)
  {
    ha_rows num_rows_in_partition= 0;
    if (cursor.fetch())
      return;
    num_rows_in_partition++;

    /* Walk to the end of the partition, find how many rows there are. */
    while (!cursor.next())
      num_rows_in_partition++;

    List_iterator_fast<Item_sum> it(sum_functions);
    Item_sum* item;
    while ((item= it++))
    {
      Item_sum_window_with_row_count* item_with_row_count =
        static_cast<Item_sum_window_with_row_count *>(item);
      item_with_row_count->set_row_count(num_rows_in_partition);
    }
  }

  ha_rows get_curr_rownum() const
  {
    return cursor.get_rownum();
  }
};

/////////////////////////////////////////////////////////////////////////////
// ROWS-type frame bounds
/////////////////////////////////////////////////////////////////////////////
/*
  ROWS $n PRECEDING frame bound

*/
class Frame_n_rows_preceding : public Frame_cursor
{
  /* Whether this is top of the frame or bottom */
  const bool is_top_bound;
  const ha_rows n_rows;

  /* Number of rows that we need to skip before our cursor starts moving */
  ha_rows n_rows_behind;

  Table_read_cursor cursor;
public:
  Frame_n_rows_preceding(bool is_top_bound_arg, ha_rows n_rows_arg) :
    is_top_bound(is_top_bound_arg), n_rows(n_rows_arg), n_rows_behind(0)
  {}

  void init(READ_RECORD *info)
  {
    cursor.init(info);
  }

  void next_partition(ha_rows rownum)
  {
    /*
      Position our cursor to point at the first row in the new partition
      (for rownum=0, it is already there, otherwise, it lags behind)
    */
    cursor.move_to(rownum);
    /* Cursor is in the same spot as current row. */
    n_rows_behind= 0;

    /*
      Suppose the bound is ROWS 2 PRECEDING, and current row is row#n:
        ...
        n-3
        n-2 --- bound row
        n-1
         n  --- current_row
        ...
       The bound should point at row #(n-2). Bounds are inclusive, so
        - bottom bound should add row #(n-2) into the window function
        - top bound should remove row (#n-3) from the window function.
    */
    move_cursor_if_possible();

  }

  void next_row()
  {
    n_rows_behind++;
    move_cursor_if_possible();
  }

  bool is_outside_computation_bounds() const
  {
    /* As a bottom boundary, rows have not yet been added. */
    if (!is_top_bound && n_rows - n_rows_behind)
      return true;
    return false;
  }

  ha_rows get_curr_rownum() const
  {
    return cursor.get_rownum();
  }

private:
  void move_cursor_if_possible()
  {
    longlong rows_difference= n_rows - n_rows_behind;
    if (rows_difference > 0) /* We still have to wait. */
      return;

      /* The cursor points to the first row in the frame. */
    if (rows_difference == 0)
    {
      if (!is_top_bound)
      {
        cursor.fetch();
        add_value_to_items();
      }
      /* For top bound we don't have to remove anything as nothing was added. */
      return;
    }

    /* We need to catch up by one row. */
    DBUG_ASSERT(rows_difference == -1);

    if (is_top_bound)
    {
      cursor.fetch();
      remove_value_from_items();
      cursor.next();
    }
    else
    {
      cursor.next();
      cursor.fetch();
      add_value_to_items();
    }
    /* We've advanced one row. We are no longer behind. */
    n_rows_behind--;
  }
};


/*
  ROWS ... CURRENT ROW, Bottom bound.

  This case is moved to separate class because here we don't need to maintain
  our own cursor, or check for partition bound.
*/

class Frame_rows_current_row_bottom : public Frame_cursor
{
public:

  Frame_rows_current_row_bottom() : curr_rownum(0) {}

  void pre_next_partition(ha_rows rownum)
  {
    add_value_to_items();
    curr_rownum= rownum;
  }

  void next_partition(ha_rows rownum) {}

  void pre_next_row()
  {
    /* Temp table's current row is current_row. Add it to the window func */
    add_value_to_items();
  }

  void next_row()
  {
    curr_rownum++;
  };

  ha_rows get_curr_rownum() const
  {
    return curr_rownum;
  }

private:
  ha_rows curr_rownum;
};


/*
  ROWS-type CURRENT ROW, top bound.

  This serves for processing "ROWS BETWEEN CURRENT ROW AND ..." frames.

      n-1
       n  --+  --- current_row, and top frame bound
      n+1   |
      ...   |

  when the current_row moves to row #n, this frame bound should remove the
  row #(n-1) from the window function.

  In other words, we need what "ROWS PRECEDING 0" provides.
*/
class Frame_rows_current_row_top: public Frame_n_rows_preceding

{
public:
  Frame_rows_current_row_top() :
    Frame_n_rows_preceding(true /*top*/, 0 /* n_rows */)
  {}
};


/*
  ROWS $n FOLLOWING frame bound.
*/

class Frame_n_rows_following : public Frame_cursor
{
  /* Whether this is top of the frame or bottom */
  const bool is_top_bound;
  const ha_rows n_rows;

  Partition_read_cursor cursor;
  bool at_partition_end;
public:
  Frame_n_rows_following(THD *thd,
            SQL_I_List<ORDER> *partition_list,
            SQL_I_List<ORDER> *order_list,
            bool is_top_bound_arg, ha_rows n_rows_arg) :
    is_top_bound(is_top_bound_arg), n_rows(n_rows_arg),
    cursor(thd, partition_list)
  {
  }

  void init(READ_RECORD *info)
  {
    cursor.init(info);
    at_partition_end= false;
  }

  void pre_next_partition(ha_rows rownum)
  {
    at_partition_end= false;

    cursor.on_next_partition(rownum);
  }

  /* Move our cursor to be n_rows ahead.  */
  void next_partition(ha_rows rownum)
  {
    if (is_top_bound)
      next_part_top(rownum);
    else
      next_part_bottom(rownum);
  }

  void next_row()
  {
    if (is_top_bound)
      next_row_top();
    else
      next_row_bottom();
  }

  bool is_outside_computation_bounds() const
  {
    /*
       The top bound can go over the current partition. In this case,
       the sum function has 0 values added to it.
    */
    if (at_partition_end && is_top_bound)
      return true;
    return false;
  }

  ha_rows get_curr_rownum() const
  {
    return cursor.get_rownum();
  }

private:
  void next_part_top(ha_rows rownum)
  {
    for (ha_rows i= 0; i < n_rows; i++)
    {
      if (cursor.fetch())
        break;
      remove_value_from_items();
      if (cursor.next())
        at_partition_end= true;
    }
  }

  void next_part_bottom(ha_rows rownum)
  {
    if (cursor.fetch())
      return;
    add_value_to_items();

    for (ha_rows i= 0; i < n_rows; i++)
    {
      if (cursor.next())
      {
        at_partition_end= true;
        break;
      }
      add_value_to_items();
    }
    return;
  }

  void next_row_top()
  {
    if (cursor.fetch()) // PART END OR FAILURE
    {
      at_partition_end= true;
      return;
    }
    remove_value_from_items();
    if (cursor.next())
    {
      at_partition_end= true;
      return;
    }
  }

  void next_row_bottom()
  {
    if (at_partition_end)
      return;

    if (cursor.next())
    {
      at_partition_end= true;
      return;
    }

    add_value_to_items();

  }
};

/*
  A cursor that performs a table scan between two indices. The indices
  are provided by the two cursors representing the top and bottom bound
  of the window function's frame definition.

  Each scan clears the sum function.

  NOTE:
    The cursor does not alter the top and bottom cursors.
    This type of cursor is expensive computational wise. This is only to be
    used when the sum functions do not support removal.
*/
class Frame_scan_cursor : public Frame_cursor
{
public:
  Frame_scan_cursor(const Frame_cursor &top_bound,
                    const Frame_cursor &bottom_bound) :
    top_bound(top_bound), bottom_bound(bottom_bound) {}

  void init(READ_RECORD *info)
  {
    cursor.init(info);
  }

  void pre_next_partition(ha_rows rownum)
  {
    /* TODO(cvicentiu) Sum functions get cleared on next partition anyway during
       the window function computation algorithm. Either perform this only in
       cursors, or remove it from pre_next_partition.
    */
    curr_rownum= rownum;
    clear_sum_functions();
  }

  void next_partition(ha_rows rownum)
  {
    compute_values_for_current_row();
  }

  void pre_next_row()
  {
    clear_sum_functions();
  }

  void next_row()
  {
    curr_rownum++;
    compute_values_for_current_row();
  }

  ha_rows get_curr_rownum() const
  {
    return curr_rownum;
  }

private:
  const Frame_cursor &top_bound;
  const Frame_cursor &bottom_bound;
  Table_read_cursor cursor;
  ha_rows curr_rownum;

  /* Scan the rows between the top bound and bottom bound. Add all the values
     between them, top bound row  and bottom bound row inclusive. */
  void compute_values_for_current_row()
  {
    if (top_bound.is_outside_computation_bounds() ||
        bottom_bound.is_outside_computation_bounds())
      return;

    ha_rows start_rownum= top_bound.get_curr_rownum();
    ha_rows bottom_rownum= bottom_bound.get_curr_rownum();
    DBUG_PRINT("info", ("COMPUTING (%llu %llu)", start_rownum, bottom_rownum));

    cursor.move_to(start_rownum);

    for (ha_rows idx= start_rownum; idx <= bottom_rownum; idx++)
    {
      if (cursor.fetch()) //EOF
        break;
      add_value_to_items();
      if (cursor.next()) // EOF
        break;
    }
  }
};

/* A cursor that follows a target cursor. Each time a new row is added,
   the window functions are cleared and only have the row at which the target
   is point at added to them.

   The window functions are cleared if the bounds or the position cursors are
   outside computational bounds.
*/
class Frame_positional_cursor : public Frame_cursor
{
 public:
  Frame_positional_cursor(const Frame_cursor &position_cursor) :
    position_cursor(position_cursor), top_bound(NULL),
    bottom_bound(NULL), offset(NULL), overflowed(false),
    negative_offset(false) {}

  Frame_positional_cursor(const Frame_cursor &position_cursor,
                          const Frame_cursor &top_bound,
                          const Frame_cursor &bottom_bound,
                          Item &offset,
                          bool negative_offset) :
    position_cursor(position_cursor), top_bound(&top_bound),
    bottom_bound(&bottom_bound), offset(&offset),
    negative_offset(negative_offset) {}

  void init(READ_RECORD *info)
  {
    cursor.init(info);
  }

  void pre_next_partition(ha_rows rownum)
  {
    /* The offset is dependant on the current row values. We can only get
     * it here accurately. When fetching other rows, it changes. */
    save_offset_value();
  }

  void next_partition(ha_rows rownum)
  {
    save_positional_value();
  }

  void pre_next_row()
  {
    /* The offset is dependant on the current row values. We can only get
     * it here accurately. When fetching other rows, it changes. */
    save_offset_value();
  }

  void next_row()
  {
    save_positional_value();
  }

  ha_rows get_curr_rownum() const
  {
    return position_cursor.get_curr_rownum();
  }

private:
  /* Check if a our position is within bounds.
   * The position is passed as a parameter to avoid recalculating it. */
  bool position_is_within_bounds()
  {
    if (!offset)
      return !position_cursor.is_outside_computation_bounds();

    if (overflowed)
      return false;

    /* No valid bound to compare to. */
    if (position_cursor.is_outside_computation_bounds() ||
        top_bound->is_outside_computation_bounds() ||
        bottom_bound->is_outside_computation_bounds())
      return false;

    /* We are over the bound. */
    if (position < top_bound->get_curr_rownum())
      return false;
    if (position > bottom_bound->get_curr_rownum())
      return false;

    return true;
  }

  /* Get the current position, accounting for the offset value, if present.
     NOTE: This function does not check over/underflow.
  */
  void get_current_position()
  {
    position = position_cursor.get_curr_rownum();
    overflowed= false;
    if (offset)
    {
      if (offset_value < 0 &&
          position + offset_value > position)
      {
        overflowed= true;
      }
      if (offset_value > 0 &&
          position + offset_value < position)
      {
        overflowed= true;
      }
      position += offset_value;
    }
  }

  void save_offset_value()
  {
    if (offset)
      offset_value= offset->val_int() * (negative_offset ? -1 : 1);
    else
      offset_value= 0;
  }

  void save_positional_value()
  {
    get_current_position();
    if (!position_is_within_bounds())
      clear_sum_functions();
    else
    {
      cursor.move_to(position);
      cursor.fetch();
      add_value_to_items();
    }
  }

  const Frame_cursor &position_cursor;
  const Frame_cursor *top_bound;
  const Frame_cursor *bottom_bound;
  Item *offset;
  Table_read_cursor cursor;
  ha_rows position;
  longlong offset_value;
  bool overflowed;

  bool negative_offset;
};


/*
  Get a Frame_cursor for a frame bound. This is a "factory function".
*/
Frame_cursor *get_frame_cursor(THD *thd, Window_spec *spec, bool is_top_bound)
{
  Window_frame *frame= spec->window_frame;
  if (!frame)
  {
    /*
      The docs say this about the lack of frame clause:

        Let WD be a window structure descriptor.
        ...
        If WD has no window framing clause, then
        Case:
        i) If the window ordering clause of WD is not present, then WF is the
           window partition of R.
        ii) Otherwise, WF consists of all rows of the partition of R that
           precede R or are peers of R in the window ordering of the window
           partition defined by the window ordering clause.

        For case #ii, the frame bounds essentially are "RANGE BETWEEN UNBOUNDED
        PRECEDING AND CURRENT ROW".
        For the case #i, without ordering clause all rows are considered peers,
        so again the same frame bounds can be used.
    */
    if (is_top_bound)
      return new Frame_unbounded_preceding(thd,
                                           spec->partition_list,
                                           spec->order_list);
    else
      return new Frame_range_current_row_bottom(thd,
                                                spec->partition_list,
                                                spec->order_list);
  }

  Window_frame_bound *bound= is_top_bound? frame->top_bound :
                                           frame->bottom_bound;

  if (bound->precedence_type == Window_frame_bound::PRECEDING ||
      bound->precedence_type == Window_frame_bound::FOLLOWING)
  {
    bool is_preceding= (bound->precedence_type ==
                        Window_frame_bound::PRECEDING);

    if (bound->offset == NULL) /* this is UNBOUNDED */
    {
      /* The following serve both RANGE and ROWS: */
      if (is_preceding)
        return new Frame_unbounded_preceding(thd,
                                             spec->partition_list,
                                             spec->order_list);

      return new Frame_unbounded_following(thd,
                                           spec->partition_list,
                                           spec->order_list);
    }

    if (frame->units == Window_frame::UNITS_ROWS)
    {
      ha_rows n_rows= bound->offset->val_int();
      /* These should be handled in the parser */
      DBUG_ASSERT(!bound->offset->null_value);
      DBUG_ASSERT((longlong) n_rows >= 0);
      if (is_preceding)
        return new Frame_n_rows_preceding(is_top_bound, n_rows);

      return new Frame_n_rows_following(
          thd, spec->partition_list, spec->order_list,
          is_top_bound, n_rows);
    }
    else
    {
      if (is_top_bound)
        return new Frame_range_n_top(
            thd, spec->partition_list, spec->order_list,
            is_preceding, bound->offset);

      return new Frame_range_n_bottom(thd,
          spec->partition_list, spec->order_list,
          is_preceding, bound->offset);
    }
  }

  if (bound->precedence_type == Window_frame_bound::CURRENT)
  {
    if (frame->units == Window_frame::UNITS_ROWS)
    {
      if (is_top_bound)
        return new Frame_rows_current_row_top;

      return new Frame_rows_current_row_bottom;
    }
    else
    {
      if (is_top_bound)
        return new Frame_range_current_row_top(
            thd, spec->partition_list, spec->order_list);

      return new Frame_range_current_row_bottom(
          thd, spec->partition_list, spec->order_list);
    }
  }
  return NULL;
}

static
void add_special_frame_cursors(THD *thd, Cursor_manager *cursor_manager,
                               Item_window_func *window_func)
{
  Window_spec *spec= window_func->window_spec;
  Item_sum *item_sum= window_func->window_func();
  DBUG_PRINT("info", ("Get arg count: %d", item_sum->get_arg_count()));
  Frame_cursor *fc;
  switch (item_sum->sum_func())
  {
    case Item_sum::CUME_DIST_FUNC:
      fc= new Frame_unbounded_preceding(thd,
                                        spec->partition_list,
                                        spec->order_list);
      fc->add_sum_func(item_sum);
      cursor_manager->add_cursor(fc);
      fc= new Frame_range_current_row_bottom(thd,
                                             spec->partition_list,
                                             spec->order_list);
      fc->add_sum_func(item_sum);
      cursor_manager->add_cursor(fc);
      break;
    case Item_sum::LEAD_FUNC:
    case Item_sum::LAG_FUNC:
    {
      Frame_cursor *bottom_bound= new Frame_unbounded_following(thd,
                                                                spec->partition_list,
                                                                spec->order_list);
      Frame_cursor *top_bound= new Frame_unbounded_preceding(thd,
                                                             spec->partition_list,
                                                             spec->order_list);
      Frame_cursor *current_row_pos= new Frame_rows_current_row_bottom;
      cursor_manager->add_cursor(bottom_bound);
      cursor_manager->add_cursor(top_bound);
      cursor_manager->add_cursor(current_row_pos);
      DBUG_ASSERT(item_sum->fixed);
      bool negative_offset= item_sum->sum_func() == Item_sum::LAG_FUNC;
      fc= new Frame_positional_cursor(*current_row_pos,
                                      *top_bound, *bottom_bound,
                                      *item_sum->get_arg(1),
                                      negative_offset);
      fc->add_sum_func(item_sum);
      cursor_manager->add_cursor(fc);
      break;
    }
    case Item_sum::FIRST_VALUE_FUNC:
    {
      Frame_cursor *bottom_bound= get_frame_cursor(thd, spec, false);
      Frame_cursor *top_bound= get_frame_cursor(thd, spec, true);
      cursor_manager->add_cursor(bottom_bound);
      cursor_manager->add_cursor(top_bound);
      DBUG_ASSERT(item_sum->fixed);
      Item *offset_item= new (thd->mem_root) Item_int(thd, 0);
      offset_item->fix_fields(thd, &offset_item);
      fc= new Frame_positional_cursor(*top_bound,
                                      *top_bound, *bottom_bound,
                                      *offset_item, false);
      fc->add_sum_func(item_sum);
      cursor_manager->add_cursor(fc);
      break;
    }
    case Item_sum::LAST_VALUE_FUNC:
    {
      Frame_cursor *bottom_bound= get_frame_cursor(thd, spec, false);
      Frame_cursor *top_bound= get_frame_cursor(thd, spec, true);
      cursor_manager->add_cursor(bottom_bound);
      cursor_manager->add_cursor(top_bound);
      DBUG_ASSERT(item_sum->fixed);
      Item *offset_item= new (thd->mem_root) Item_int(thd, 0);
      offset_item->fix_fields(thd, &offset_item);
      fc= new Frame_positional_cursor(*bottom_bound,
                                      *top_bound, *bottom_bound,
                                      *offset_item, false);
      fc->add_sum_func(item_sum);
      cursor_manager->add_cursor(fc);
      break;
    }
    case Item_sum::NTH_VALUE_FUNC:
    {
      Frame_cursor *bottom_bound= get_frame_cursor(thd, spec, false);
      Frame_cursor *top_bound= get_frame_cursor(thd, spec, true);
      cursor_manager->add_cursor(bottom_bound);
      cursor_manager->add_cursor(top_bound);
      DBUG_ASSERT(item_sum->fixed);
      Item *int_item= new (thd->mem_root) Item_int(thd, 1);
      Item *offset_func= new (thd->mem_root)
                              Item_func_minus(thd, item_sum->get_arg(1),
                                              int_item);
      offset_func->fix_fields(thd, &offset_func);
      fc= new Frame_positional_cursor(*top_bound,
                                      *top_bound, *bottom_bound,
                                      *offset_func, false);
      fc->add_sum_func(item_sum);
      cursor_manager->add_cursor(fc);
      break;
    }
    default:
      fc= new Frame_unbounded_preceding(
              thd, spec->partition_list, spec->order_list);
      fc->add_sum_func(item_sum);
      cursor_manager->add_cursor(fc);

      fc= new Frame_rows_current_row_bottom;
      fc->add_sum_func(item_sum);
      cursor_manager->add_cursor(fc);
  }
}


static bool is_computed_with_remove(Item_sum::Sumfunctype sum_func)
{
  switch (sum_func)
  {
    case Item_sum::CUME_DIST_FUNC:
    case Item_sum::ROW_NUMBER_FUNC:
    case Item_sum::RANK_FUNC:
    case Item_sum::DENSE_RANK_FUNC:
    case Item_sum::NTILE_FUNC:
    case Item_sum::FIRST_VALUE_FUNC:
    case Item_sum::LAST_VALUE_FUNC:
      return false;
    default:
      return true;
  }
}
/*
   Create required frame cursors for the list of window functions.
   Register all functions to their appropriate cursors.
   If the window functions share the same frame specification,
   those window functions will be registered to the same cursor.
*/
void get_window_functions_required_cursors(
    THD *thd,
    List<Item_window_func>& window_functions,
    List<Cursor_manager> *cursor_managers)
{
  List_iterator_fast<Item_window_func> it(window_functions);
  Item_window_func* item_win_func;
  Item_sum *sum_func;
  while ((item_win_func= it++))
  {
    Cursor_manager *cursor_manager = new Cursor_manager();
    sum_func = item_win_func->window_func();
    Frame_cursor *fc;
    /*
      Some window functions require the partition size for computing values.
      Add a cursor that retrieves it as the first one in the list if necessary.
    */
    if (item_win_func->requires_partition_size())
    {
      fc= new Frame_unbounded_following_set_count(thd,
                item_win_func->window_spec->partition_list,
                item_win_func->window_spec->order_list);
      fc->add_sum_func(sum_func);
      cursor_manager->add_cursor(fc);
    }

    /*
      If it is not a regular window function that follows frame specifications,
      and/or specific cursors are required. ROW_NUM, RANK, NTILE and others
      follow such rules. Check is_frame_prohibited check for the full list.

      TODO(cvicentiu) This approach is messy. Every time a function allows
      computation in a certain way, we have to add an extra method to this
      factory function. It is better to have window functions output
      their own cursors, as needed. This way, the logic is bound
      only to the implementation of said window function. Regular aggregate
      functions can keep the default frame generating code, overwrite it or
      add to it.
    */
    if (item_win_func->is_frame_prohibited() ||
        item_win_func->requires_special_cursors())
    {
      add_special_frame_cursors(thd, cursor_manager, item_win_func);
      cursor_managers->push_back(cursor_manager);
      continue;
    }

    Frame_cursor *frame_bottom= get_frame_cursor(thd,
        item_win_func->window_spec, false);
    Frame_cursor *frame_top= get_frame_cursor(thd,
        item_win_func->window_spec, true);

    frame_bottom->add_sum_func(sum_func);
    frame_top->add_sum_func(sum_func);

    /*
       The order of these cursors is important. A sum function
       must first add values (via frame_bottom) then remove them via
       frame_top. Removing items first doesn't make sense in the case of all
       window functions.
    */
    cursor_manager->add_cursor(frame_bottom);
    cursor_manager->add_cursor(frame_top);
    if (is_computed_with_remove(sum_func->sum_func()) &&
        !sum_func->supports_removal())
    {
      frame_bottom->set_no_action();
      frame_top->set_no_action();
      Frame_cursor *scan_cursor= new Frame_scan_cursor(*frame_top,
                                                       *frame_bottom);
      scan_cursor->add_sum_func(sum_func);
      cursor_manager->add_cursor(scan_cursor);

    }
    cursor_managers->push_back(cursor_manager);
  }
}

/**
  Helper function that takes a list of window functions and writes
  their values in the current table record.
*/
static
bool save_window_function_values(List<Item_window_func>& window_functions,
                                 TABLE *tbl, uchar *rowid_buf)
{
  List_iterator_fast<Item_window_func> iter(window_functions);
  JOIN_TAB *join_tab= tbl->reginfo.join_tab;
  tbl->file->ha_rnd_pos(tbl->record[0], rowid_buf);
  store_record(tbl, record[1]);
  while (Item_window_func *item_win= iter++)
    item_win->save_in_field(item_win->result_field, true);

  /*
    In case we have window functions present, an extra step is required
    to compute all the fields from the temporary table.
    In case we have a compound expression such as: expr + expr,
    where one of the terms has a window function inside it, only
    after computing window function values we actually know the true
    final result of the compounded expression.

    Go through all the func items and save their values once again in the
    corresponding temp table fields. Do this for each row in the table.

    This needs to be done earlier because ORDER BY clause can also have
    a window function, so we need to make sure all the fields of the temp.table
    are updated before we do the filesort. So is best to update the other fields
    that contain the window functions along with the computation of window
    functions.
  */

  Item **func_ptr= join_tab->tmp_table_param->items_to_copy;
  Item *func;
  for (; (func = *func_ptr) ; func_ptr++)
  {
    if (func->with_window_func && func->type() != Item::WINDOW_FUNC_ITEM)
      func->save_in_result_field(true);
  }

  int err= tbl->file->ha_update_row(tbl->record[1], tbl->record[0]);
  if (err && err != HA_ERR_RECORD_IS_THE_SAME)
    return true;

  return false;
}

/*
  TODO(cvicentiu) update this comment to reflect the new execution.

  Streamed window function computation with window frames.

  We make a single pass over the ordered temp.table, but we're using three
  cursors:
   - current row - the row that we're computing window func value for)
   - start_bound - the start of the frame
   - bottom_bound   - the end of the frame

  All three cursors move together.

  @todo
    Provided bounds have their 'cursors'... is it better to re-clone their
    cursors or re-position them onto the current row?

  @detail
    ROWS BETWEEN 3 PRECEDING  -- frame start
              AND 3 FOLLOWING  -- frame end

                                    /------ frame end (aka BOTTOM)
    Dataset start                   |
     --------====*=======[*]========*========-------->> dataset end
                 |        \  
                 |         +-------- current row
                 |
                 \-------- frame start ("TOP")

    - frame_end moves forward and adds rows into the aggregate function.
    - frame_start follows behind and removes rows from the aggregate function.
    - current_row is the row where the value of aggregate function is stored.

  @TODO:  Only the first cursor needs to check for run-out-of-partition
  condition (Others can catch up by counting rows?)

*/
bool compute_window_func(THD *thd,
                         List<Item_window_func>& window_functions,
                         List<Cursor_manager>& cursor_managers,
                         TABLE *tbl,
                         SORT_INFO *filesort_result)
{
  List_iterator_fast<Item_window_func> iter_win_funcs(window_functions);
  List_iterator_fast<Cursor_manager> iter_cursor_managers(cursor_managers);
  uint err;

  READ_RECORD info;

  if (init_read_record(&info, current_thd, tbl, NULL/*select*/, filesort_result,
                       0, 1, FALSE))
    return true;

  Cursor_manager *cursor_manager;
  while ((cursor_manager= iter_cursor_managers++))
    cursor_manager->initialize_cursors(&info);

  /* One partition tracker for each window function. */
  List<Group_bound_tracker> partition_trackers;
  Item_window_func *win_func;
  while ((win_func= iter_win_funcs++))
  {
    Group_bound_tracker *tracker= new Group_bound_tracker(thd,
                                        win_func->window_spec->partition_list);
    // TODO(cvicentiu) This should be removed and placed in constructor.
    tracker->init();
    partition_trackers.push_back(tracker);
  }

  List_iterator_fast<Group_bound_tracker> iter_part_trackers(partition_trackers);
  ha_rows rownum= 0;
  uchar *rowid_buf= (uchar*) my_malloc(tbl->file->ref_length, MYF(0));

  while (true)
  {
    if ((err= info.read_record(&info)))
      break; // End of file.

    /* Remember current row so that we can restore it before computing
       each window function. */
    tbl->file->position(tbl->record[0]);
    memcpy(rowid_buf, tbl->file->ref, tbl->file->ref_length);

    iter_win_funcs.rewind();
    iter_part_trackers.rewind();
    iter_cursor_managers.rewind();

    Group_bound_tracker *tracker;
    while ((win_func= iter_win_funcs++) &&
           (tracker= iter_part_trackers++) &&
           (cursor_manager= iter_cursor_managers++))
    {
      if (tracker->check_if_next_group() || (rownum == 0))
      {
        /* TODO(cvicentiu)
           Clearing window functions should happen through cursors. */
        win_func->window_func()->clear();
        cursor_manager->notify_cursors_partition_changed(rownum);
      }
      else
      {
        cursor_manager->notify_cursors_next_row();
      }
      /* Return to current row after notifying cursors for each window
         function. */
      tbl->file->ha_rnd_pos(tbl->record[0], rowid_buf);
    }

    /* We now have computed values for each window function. They can now
       be saved in the current row. */
    save_window_function_values(window_functions, tbl, rowid_buf);

    rownum++;
  }

  my_free(rowid_buf);
  partition_trackers.delete_elements();
  end_read_record(&info);

  return false;
}

/* Make a list that is a concation of two lists of ORDER elements */

static ORDER* concat_order_lists(MEM_ROOT *mem_root, ORDER *list1, ORDER *list2)
{
  if (!list1)
  {
    list1= list2;
    list2= NULL;
  }

  ORDER *res= NULL; // first element in the new list
  ORDER *prev= NULL; // last element in the new list 
  ORDER *cur_list= list1; // this goes through list1, list2
  while (cur_list)
  {
    for (ORDER *cur= cur_list; cur; cur= cur->next)
    {
      ORDER *copy= (ORDER*)alloc_root(mem_root, sizeof(ORDER));
      memcpy(copy, cur, sizeof(ORDER));
      if (prev)
        prev->next= copy;
      prev= copy;
      if (!res)
        res= copy;
    }

    cur_list= (cur_list == list1)? list2: NULL;
  }

  if (prev)
    prev->next= NULL;

  return res;
}

bool Window_func_runner::add_function_to_run(Item_window_func *win_func)
{

  Item_sum *sum_func= win_func->window_func();
  sum_func->setup_window_func(current_thd, win_func->window_spec);

  Item_sum::Sumfunctype type= win_func->window_func()->sum_func();

  switch (type)
  {
    /* Distinct is not yet supported. */
    case Item_sum::GROUP_CONCAT_FUNC:
      my_error(ER_NOT_SUPPORTED_YET, MYF(0),
               "GROUP_CONCAT() aggregate as window function");
      return true;
    case Item_sum::SUM_DISTINCT_FUNC:
      my_error(ER_NOT_SUPPORTED_YET, MYF(0),
               "SUM(DISTINCT) aggregate as window function");
      return true;
    case Item_sum::AVG_DISTINCT_FUNC:
      my_error(ER_NOT_SUPPORTED_YET, MYF(0),
               "AVG(DISTINCT) aggregate as window function");
      return true;
    case Item_sum::COUNT_DISTINCT_FUNC:
      my_error(ER_NOT_SUPPORTED_YET, MYF(0),
               "COUNT(DISTINCT) aggregate as window function");
      return true;
    default:
      break;
  }

  return window_functions.push_back(win_func);
}


/*
  Compute the value of window function for all rows.
*/
bool Window_func_runner::exec(THD *thd, TABLE *tbl, SORT_INFO *filesort_result)
{
  List_iterator_fast<Item_window_func> it(window_functions);
  Item_window_func *win_func;
  while ((win_func= it++))
  {
    win_func->set_phase_to_computation();
    // TODO(cvicentiu) Setting the aggregator should probably be done during
    // setup of Window_funcs_sort.
    win_func->window_func()->set_aggregator(Aggregator::SIMPLE_AGGREGATOR);
  }
  it.rewind();

  List<Cursor_manager> cursor_managers;
  get_window_functions_required_cursors(thd, window_functions,
                                        &cursor_managers);

  /* Go through the sorted array and compute the window function */
  bool is_error= compute_window_func(thd,
                                     window_functions,
                                     cursor_managers,
                                     tbl, filesort_result);
  while ((win_func= it++))
  {
    win_func->set_phase_to_retrieval();
  }

  cursor_managers.delete_elements();

  return is_error;
}


bool Window_funcs_sort::exec(JOIN *join, bool keep_filesort_result)
{
  THD *thd= join->thd;
  JOIN_TAB *join_tab= join->join_tab + join->total_join_tab_cnt();

  /* Sort the table based on the most specific sorting criteria of
     the window functions. */
  if (create_sort_index(thd, join, join_tab, filesort))
    return true;

  TABLE *tbl= join_tab->table;
  SORT_INFO *filesort_result= join_tab->filesort_result;

  bool is_error= runner.exec(thd, tbl, filesort_result);

  if (!keep_filesort_result)
  {
    delete join_tab->filesort_result;
    join_tab->filesort_result= NULL;
  }
  return is_error;
}


bool Window_funcs_sort::setup(THD *thd, SQL_SELECT *sel,
                              List_iterator<Item_window_func> &it,
                              JOIN_TAB *join_tab)
{
  Window_spec *spec;
  Item_window_func *win_func= it.peek();  
  Item_window_func *win_func_with_longest_order= NULL;
  int longest_order_elements= -1;

  /* The iterator should point to a valid function at the start of execution. */
  DBUG_ASSERT(win_func);
  do
  {
    spec= win_func->window_spec;
    int win_func_order_elements= spec->partition_list->elements +
                                  spec->order_list->elements;
    if (win_func_order_elements > longest_order_elements)
    {
      win_func_with_longest_order= win_func;
      longest_order_elements= win_func_order_elements;
    }
    if (runner.add_function_to_run(win_func))
      return true;
    it++;
    win_func= it.peek();
  } while (win_func && !(win_func->marker & SORTORDER_CHANGE_FLAG));

  /*
    The sort criteria must be taken from the last win_func in the group of
    adjacent win_funcs that do not have SORTORDER_CHANGE_FLAG. This is
    because the sort order must be the most specific sorting criteria defined
    within the window function group. This ensures that we sort the table
    in a way that the result is valid for all window functions belonging to
    this Window_funcs_sort.
  */
  spec= win_func_with_longest_order->window_spec;

  ORDER* sort_order= concat_order_lists(thd->mem_root, 
                                        spec->partition_list->first,
                                        spec->order_list->first);
  if (sort_order == NULL) // No partition or order by clause.
  {
    /* TODO(cvicentiu) This is used as a way to allow an empty OVER ()
       clause for window functions. However, a better approach is
       to not call Filesort at all in this case and just read whatever order
       the temporary table has.
       Due to cursors not working for out_of_memory cases (yet!), we have to run
       filesort to generate a sort buffer of the results.
       In this case we sort by the first field of the temporary table.
       We should have this field available, even if it is a window_function
       field. We don't care of the particular sorting result in this case.
     */
    ORDER *order= (ORDER *)alloc_root(thd->mem_root, sizeof(ORDER));
    memset(order, 0, sizeof(*order));
    Item *item= new (thd->mem_root) Item_temptable_field(thd,
                                                    join_tab->table->field[0]);
    order->item= (Item **)alloc_root(thd->mem_root, 2 * sizeof(Item *));
    order->item[1]= NULL;
    order->item[0]= item;
    order->field= join_tab->table->field[0];
    sort_order= order;
  }
  filesort= new (thd->mem_root) Filesort(sort_order, HA_POS_ERROR, true, NULL);

  /* Apply the same condition that the subsequent sort has. */
  filesort->select= sel;

  return false;
}


bool Window_funcs_computation::setup(THD *thd,
                                     List<Item_window_func> *window_funcs,
                                     JOIN_TAB *tab)
{
  order_window_funcs_by_window_specs(window_funcs);

  SQL_SELECT *sel= NULL;
  /*
     If the tmp table is filtered during sorting
     (ex: SELECT with HAVING && ORDER BY), we must make sure to keep the
     filtering conditions when we perform sorting for window function
     computation.
  */
  if (tab->filesort && tab->filesort->select)
  {
    sel= tab->filesort->select;
    DBUG_ASSERT(!sel->quick);
  }

  Window_funcs_sort *srt;
  List_iterator<Item_window_func> iter(*window_funcs);
  while (iter.peek())
  {
    if (!(srt= new Window_funcs_sort()) ||
        srt->setup(thd, sel, iter, tab))
    {
      return true;
    }
    win_func_sorts.push_back(srt, thd->mem_root);
  }
  return false;
}


bool Window_funcs_computation::exec(JOIN *join, bool keep_last_filesort_result)
{
  List_iterator<Window_funcs_sort> it(win_func_sorts);
  Window_funcs_sort *srt;
  uint counter= 0; /* Count how many sorts we've executed. */
  /* Execute each sort */
  while ((srt = it++))
  {
    counter++;
    bool keep_filesort_result= keep_last_filesort_result &&
                               counter == win_func_sorts.elements;
    if (srt->exec(join, keep_filesort_result))
      return true;
  }
  return false;
}


void Window_funcs_computation::cleanup()
{
  List_iterator<Window_funcs_sort> it(win_func_sorts);
  Window_funcs_sort *srt;
  while ((srt = it++))
  {
    srt->cleanup();
    delete srt;
  }
}


Explain_aggr_window_funcs*
Window_funcs_computation::save_explain_plan(MEM_ROOT *mem_root, 
                                                 bool is_analyze)
{
  Explain_aggr_window_funcs *xpl= new Explain_aggr_window_funcs;
  List_iterator<Window_funcs_sort> it(win_func_sorts);
  Window_funcs_sort *srt;
  while ((srt = it++))
  {
    Explain_aggr_filesort *eaf=
      new Explain_aggr_filesort(mem_root, is_analyze, srt->filesort);
    xpl->sorts.push_back(eaf, mem_root);
  }
  return xpl;
}


bool st_select_lex::add_window_func(Item_window_func *win_func)
{
  if (parsing_place != SELECT_LIST)
    fields_in_window_functions+= win_func->window_func()->argument_count();
  return window_funcs.push_back(win_func);
}

/////////////////////////////////////////////////////////////////////////////
// Unneeded comments (will be removed when we develop a replacement for
//  the feature that was attempted here
/////////////////////////////////////////////////////////////////////////////
  /*
   TODO Get this code to set can_compute_window_function during preparation,
   not during execution.

   The reason for this is the following:
   Our single scan optimization for window functions without tmp table,
   is valid, if and only if, we only need to perform one sorting operation,
   via filesort. The cases where we need to perform one sorting operation only:

   * A select with only one window function.
   * A select with multiple window functions, but they must have their
     partition and order by clauses compatible. This means that one ordering
     is acceptable for both window functions.

       For example:
       partition by a, b, c; order by d, e    results in sorting by a b c d e.
       partition by a; order by d             results in sorting by a d.

       This kind of sorting is compatible. The less specific partition does
       not care for the order of b and c columns so it is valid if we sort
       by those in case of equality over a.

       partition by a, b; order by d, e      results in sorting by a b d e
       partition by a; order by e            results in sorting by a e

      This sorting is incompatible due to the order by clause. The partition by
      clause is compatible, (partition by a) is a prefix for (partition by a, b)
      However, order by e is not a prefix for order by d, e, thus it is not
      compatible.

    The rule for having compatible sorting is thus:
      Each partition order must contain the other window functions partitions
      prefixes, or be a prefix itself. This must hold true for all partitions.
      Analog for the order by clause.  
  */
#if 0
  List<Item_window_func> window_functions;
  SQL_I_List<ORDER> largest_partition;
  SQL_I_List<ORDER> largest_order_by;
  bool can_compute_window_live = !need_tmp;
  // Construct the window_functions item list and check if they can be
  // computed using only one sorting.
  //
  // TODO: Perhaps group functions into compatible sorting bins
  // to minimize the number of sorting passes required to compute all of them.
  while ((item= it++))
  {
    if (item->type() == Item::WINDOW_FUNC_ITEM)
    {
      Item_window_func *item_win = (Item_window_func *) item;
      window_functions.push_back(item_win);
      if (!can_compute_window_live)
        continue;  // No point checking  since we have to perform multiple sorts.
      Window_spec *spec = item_win->window_spec;
      // Having an empty partition list on one window function and a
      // not empty list on a separate window function causes the sorting
      // to be incompatible.
      //
      // Example:
      // over (partition by a, order by x) && over (order by x).
      //
      // The first function requires an ordering by a first and then by x,
      // while the seond function requires an ordering by x first.
      // The same restriction is not required for the order by clause.
      if (largest_partition.elements && !spec->partition_list.elements)
      {
        can_compute_window_live= FALSE;
        continue;
      }
      can_compute_window_live= test_if_order_compatible(largest_partition,
                                                        spec->partition_list);
      if (!can_compute_window_live)
        continue;

      can_compute_window_live= test_if_order_compatible(largest_order_by,
                                                        spec->order_list);
      if (!can_compute_window_live)
        continue;

      if (largest_partition.elements < spec->partition_list.elements)
        largest_partition = spec->partition_list;
      if (largest_order_by.elements < spec->order_list.elements)
        largest_order_by = spec->order_list;
    }
  }
  if (can_compute_window_live && window_functions.elements && table_count == 1)
  {
    ha_rows examined_rows = 0;
    ha_rows found_rows = 0;
    ha_rows filesort_retval;
    SORT_FIELD *s_order= (SORT_FIELD *) my_malloc(sizeof(SORT_FIELD) *
        (largest_partition.elements + largest_order_by.elements) + 1,
        MYF(MY_WME | MY_ZEROFILL | MY_THREAD_SPECIFIC));

    size_t pos= 0;
    for (ORDER* curr = largest_partition.first; curr; curr=curr->next, pos++)
      s_order[pos].item = *curr->item;

    for (ORDER* curr = largest_order_by.first; curr; curr=curr->next, pos++)
      s_order[pos].item = *curr->item;

    table[0]->sort.io_cache=(IO_CACHE*) my_malloc(sizeof(IO_CACHE),
                                               MYF(MY_WME | MY_ZEROFILL|
                                                   MY_THREAD_SPECIFIC));


    filesort_retval= filesort(thd, table[0], s_order,
                              (largest_partition.elements + largest_order_by.elements),
                              this->select, HA_POS_ERROR, FALSE,
                              &examined_rows, &found_rows,
                              this->explain->ops_tracker.report_sorting(thd));
    table[0]->sort.found_records= filesort_retval;

    join_tab->read_first_record = join_init_read_record;
    join_tab->records= found_rows;

    my_free(s_order);
  }
  else
#endif


