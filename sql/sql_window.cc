#include "sql_select.h"
#include "item_windowfunc.h"
#include "filesort.h"
#include "sql_base.h"
#include "sql_window.h"


bool
Window_spec::check_window_names(List_iterator_fast<Window_spec> &it)
{
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
      if (partition_list.elements)
      {
        my_error(ER_PARTITION_LIST_IN_REFERENCING_WINDOW_SPEC, MYF(0),
                 ref_name);
        return true;
      }
      if (win_spec->order_list.elements && order_list.elements)
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
      if (partition_list.elements == 0)
        partition_list= win_spec->partition_list;
      if (order_list.elements == 0)
        order_list= win_spec->order_list;
    }
  }
  if (ref_name && !referenced_win_spec)
  {
    my_error(ER_WRONG_WINDOW_SPEC_NAME, MYF(0), ref_name);
    return true;              
  }
  return false;
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


int
setup_windows(THD *thd, Ref_ptr_array ref_pointer_array, TABLE_LIST *tables,
	      List<Item> &fields, List<Item> &all_fields, 
              List<Window_spec> win_specs)
{
  Window_spec *win_spec;
  DBUG_ENTER("setup_windows");
  List_iterator<Window_spec> it(win_specs);

  /* 
    Move all unnamed specifications after the named ones.
    We could have avoided it if we had built two separate lists for
    named and unnamed specifications.
  */
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
  it.rewind();

  List_iterator_fast<Window_spec> itp(win_specs);
    
  while ((win_spec= it++))
  {
    bool hidden_group_fields;
    if (win_spec->check_window_names(itp) ||
        setup_group(thd, ref_pointer_array, tables, fields, all_fields,
                    win_spec->partition_list.first, &hidden_group_fields) ||
        setup_order(thd, ref_pointer_array, tables, fields, all_fields,
                    win_spec->order_list.first) ||
        (win_spec->window_frame && 
         win_spec->window_frame->check_frame_bounds()))
    {
      DBUG_RETURN(1);
    }
  }
  DBUG_RETURN(0);
}


/*
  Do a pass over sorted table and compute window function values.

  This function is for handling window functions that can be computed on the
  fly. Examples are RANK() and ROW_NUMBER().
*/
bool compute_window_func_values(Item_window_func *item_win, 
                                TABLE *tbl, READ_RECORD *info)
{
  int err;
  while (!(err=info->read_record(info)))
  {
    store_record(tbl,record[1]);
    
    /* 
      This will cause window function to compute its value for the
      current row :
    */
    item_win->advance_window();

    /*
      Put the new value into temptable's field
      TODO: Should this use item_win->update_field() call?
      Regular aggegate function implementations seem to implement it.
    */
    item_win->save_in_field(item_win->result_field, true);
    err= tbl->file->ha_update_row(tbl->record[1], tbl->record[0]);
    if (err && err != HA_ERR_RECORD_IS_THE_SAME)
      return true;
  }
  return false;
}

/////////////////////////////////////////////////////////////////////////////
// Window Frames support 
/////////////////////////////////////////////////////////////////////////////

// note: make rr_from_pointers static again when not need it here anymore
int rr_from_pointers(READ_RECORD *info);

/*
  A temporary way to clone READ_RECORD structures until Monty provides the real
  one.
*/
bool clone_read_record(const READ_RECORD *src, READ_RECORD *dst)
{
  DBUG_ASSERT(src->table->sort.record_pointers);
  DBUG_ASSERT(src->read_record == rr_from_pointers);
  memcpy(dst, src, sizeof(READ_RECORD));
  return false;
}

/////////////////////////////////////////////////////////////////////////////


/*
  A cursor over a sequence of rowids. One can
   - Move to next rowid
   - jump to given number in the sequence
   - Know the number of the current rowid (i.e. how many rowids have been read)
*/

class Rowid_seq_cursor
{
  uchar *cache_start;
  uchar *cache_pos;
  uchar *cache_end;
  uint ref_length;

public:
  void init(READ_RECORD *info)
  {
    cache_start= info->cache_pos;
    cache_pos=   info->cache_pos;
    cache_end=   info->cache_end;
    ref_length= info->ref_length;
  }

  virtual int get_next()
  {
    /* Allow multiple get_next() calls in EOF state*/
    if (cache_pos == cache_end)
      return -1;
    cache_pos+= ref_length;
    return 0;
  }
  
  ha_rows get_rownum()
  {
    return (cache_pos - cache_start) / ref_length;
  }

  // will be called by ROWS n FOLLOWING to catch up.
  void move_to(ha_rows row_number)
  {
    cache_pos= cache_start + row_number * ref_length;
  }
protected:
  bool at_eof() { return (cache_pos == cache_end); }

  uchar *get_last_rowid()
  {
    if (cache_pos == cache_start)
      return NULL;
    else
      return cache_pos - ref_length;
  }

  uchar *get_curr_rowid() { return cache_pos; }
};


/*
  Cursor which reads from rowid sequence and also retrieves table rows.
*/

class Table_read_cursor : public Rowid_seq_cursor
{
  /* 
    Note: we don't own *read_record, somebody else is using it.
    We only look at the constant part of it, e.g. table, record buffer, etc.
  */
  READ_RECORD *read_record;
public:

  void init(READ_RECORD *info)
  {
    Rowid_seq_cursor::init(info);
    read_record= info;
  }

  virtual int get_next()
  {
    if (at_eof())
      return -1;

    uchar* curr_rowid= get_curr_rowid();
    int res= Rowid_seq_cursor::get_next();
    if (!res)
    {
      res= read_record->table->file->ha_rnd_pos(read_record->record,
                                                curr_rowid);
    }
    return res;
  }

  bool restore_last_row()
  {
    uchar *p;
    if ((p= get_last_rowid()))
    {
      int rc= read_record->table->file->ha_rnd_pos(read_record->record, p);
      if (!rc)
        return true;
    }
    return false;
  }

  // todo: should move_to() also read row here? 
};

/*
  TODO: We should also have a cursor that reads table rows and 
  stays within the current partition.
*/

/////////////////////////////////////////////////////////////////////////////

/* A wrapper around test_if_group_changed */
class Group_bound_tracker
{
  List<Cached_item> group_fields;
public:
  void init(THD *thd, SQL_I_List<ORDER> *list)
  {
    for (ORDER *curr = list->first; curr; curr=curr->next) 
    {
      Cached_item *tmp= new_Cached_item(thd, curr->item[0], TRUE);  
      group_fields.push_back(tmp);
    }
  }

  /*
    Check if the current row is in a different group than the previous row
    this function was called for.
    The new row's group becomes the current row's group.
  */
  bool check_if_next_group()
  {
    if (test_if_group_changed(group_fields) > -1)
      return true;
    return false;
  }

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
};


/*
  Window frame bound cursor. Abstract interface.
  
  @detail
    The cursor moves within the partition that the current row is in.
    It may be ahead or behind the current row.

    The cursor also assumes that the current row moves forward through the
    partition and will move to the next adjacent partition after this one.

  @todo
  - if we want to allocate this on the MEM_ROOT we should make sure 
    it is not re-allocated for every subquery execution.
*/

class Frame_cursor : public Sql_alloc
{
public:
  virtual void init(THD *thd, READ_RECORD *info, 
                    SQL_I_List<ORDER> *partition_list,
                    SQL_I_List<ORDER> *order_list)
  {}

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
  virtual void pre_next_partition(longlong rownum, Item_sum* item){};
  virtual void next_partition(longlong rownum, Item_sum* item)=0;
  
  /*
    The current row has moved one row forward.
    Move this frame bound accordingly, and update the value of aggregate
    function as necessary.
  */
  virtual void pre_next_row(Item_sum* item){};
  virtual void next_row(Item_sum* item)=0;
  
  virtual ~Frame_cursor(){}
};

//
// RANGE-type frames
//

/*
  RANGE BETWEEN ... AND CURRENT ROW

  This is a bottom endpoint of RANGE-CURRENT ROW frame.

  It moves ahead of the current_row. It is located just in front of the first
  peer of the currrent_row.
*/

class Frame_range_current_row_bottom: public Frame_cursor
{
  Table_read_cursor cursor;
  Group_bound_tracker peer_tracker;

  bool dont_move;
public:
  void init(THD *thd, READ_RECORD *info,
            SQL_I_List<ORDER> *partition_list,
            SQL_I_List<ORDER> *order_list)
  {
    cursor.init(info);
    peer_tracker.init(thd, order_list);
  }

  void pre_next_partition(longlong rownum, Item_sum* item)
  {
    // Save the value of the current_row
    peer_tracker.check_if_next_group();
    if (rownum != 0)
      item->add(); // current row is in
  }

  void next_partition(longlong rownum, Item_sum* item)
  {
    walk_till_non_peer(item);
  }

  void pre_next_row(Item_sum* item)
  {
    // Check if our cursor is pointing at a peer of the current row.
    // If not, move forward until that becomes true
    dont_move= !peer_tracker.check_if_next_group();
    if (!dont_move)
      item->add();
  }
  // New condition: this now assumes that table's current
  // row is pointing to the current_row's position
  void next_row(Item_sum* item)
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
    walk_till_non_peer(item);
  }

private:
  void walk_till_non_peer(Item_sum* item)
  {
    /*
      Walk forward until we've met first row that's not a peer of the current
      row
    */
    while (!cursor.get_next())
    {
      if (peer_tracker.compare_with_cache())
        break;
      item->add();
    }
  }

};


/*
  RANGE BETWEEN CURRENT ROW AND ...

  This is a top endpoint of RANGE-CURRENT ROW frame.

  It moves behind the current_row. It is located right after the first peer of
  the current_row.
*/

class Frame_range_current_row_top : public Frame_cursor
{
  Group_bound_tracker bound_tracker;

  Table_read_cursor cursor;
  Group_bound_tracker peer_tracker;

  bool move;
  bool at_partition_start;
public:
  void init(THD *thd, READ_RECORD *info,
            SQL_I_List<ORDER> *partition_list,
            SQL_I_List<ORDER> *order_list)
  {
    bound_tracker.init(thd, partition_list);

    cursor.init(info);
    peer_tracker.init(thd, order_list);
  }

  void pre_next_partition(longlong rownum, Item_sum* item)
  {
    // fetch the value from the first row
    peer_tracker.check_if_next_group();
  }

  void next_partition(longlong rownum, Item_sum* item)
  {
    at_partition_start= true;
    cursor.move_to(rownum+1);
  }

  void pre_next_row(Item_sum* item)
  {
    // Check if our current row is pointing to a peer of the current row.
    // If not, move forward until that becomes true.
    move= peer_tracker.check_if_next_group();
  }

  void next_row(Item_sum* item)
  {
    bool was_at_partition_start= at_partition_start;
    at_partition_start= false;
    if (move)
    {
      if (!was_at_partition_start &&
          cursor.restore_last_row())
      {
        item->remove();
      }

      do
      {
        if (cursor.get_next())
          return;
        if (!peer_tracker.compare_with_cache())
          return;
        item->remove();
      }
      while (1);
    }
  }
};


//////////////////////////////////////////////////////////////////////////////////
/*
  UNBOUNDED PRECEDING frame bound
*/
class Frame_unbounded_preceding : public Frame_cursor
{
public:
  void next_partition(longlong rownum, Item_sum* item)
  {
    /*
      UNBOUNDED PRECEDING frame end just stays on the first row.
      We are top of the frame, so we don't need to update the sum function.
    */
  }

  void next_row(Item_sum* item)
  {
    /* Do nothing, UNBOUNDED PRECEDING frame end doesn't move. */
  }
};

/*
  UNBOUNDED FOLLOWING frame bound
*/

class Frame_unbounded_following : public Frame_cursor
{
  Table_read_cursor cursor;

  Group_bound_tracker bound_tracker;
public:
  void init(THD *thd, READ_RECORD *info, SQL_I_List<ORDER> *partition_list,
            SQL_I_List<ORDER> *order_list)
  {
    cursor.init(info);
    bound_tracker.init(thd, partition_list);
  }

  void next_partition(longlong rownum, Item_sum* item)
  {
    if (!rownum)
    {
      /* Read the first row */
      if (cursor.get_next())
        return;
    }
    /* Remember which partition we are in */
    bound_tracker.check_if_next_group();
    item->add();

    /* Walk to the end of the partition, updating the SUM function */
    while (!cursor.get_next())
    {
      if (bound_tracker.check_if_next_group())
        break;
      item->add();
    }
  }

  void next_row(Item_sum* item)
  {
    /* Do nothing, UNBOUNDED FOLLOWING frame end doesn't move */
  }
};


/*
  ROWS $n (PRECEDING|FOLLOWING) frame bound.
*/

class Frame_n_rows : public Frame_cursor
{
  /* Whether this is top of the frame or bottom */ 
  const bool is_top_bound;
  const ha_rows n_rows;
  const bool is_preceding;

  ha_rows n_rows_to_skip;

  Table_read_cursor cursor;
  bool cursor_eof; //TODO: need this still?

  Group_bound_tracker bound_tracker;
  bool at_partition_start;
  bool at_partition_end;
public:
  Frame_n_rows(bool is_top_bound_arg, bool is_preceding_arg, ha_rows n_rows_arg) :
    is_top_bound(is_top_bound_arg), n_rows(n_rows_arg), is_preceding(is_preceding_arg)
  {}

  void init(THD *thd, READ_RECORD *info, SQL_I_List<ORDER> *partition_list,
            SQL_I_List<ORDER> *order_list)
  {
    cursor.init(info);
    cursor_eof= false;
    at_partition_start= true;
    bound_tracker.init(thd, partition_list);
  }

  void next_partition(longlong rownum, Item_sum* item)
  {
    cursor_eof= false;
    at_partition_start= true;
    at_partition_end= false;
    if (is_preceding)
    {
      if (rownum != 0)
      {
        /* The cursor in "ROWS n PRECEDING" lags behind by n_rows rows. */
        cursor.move_to(rownum);
      }
      n_rows_to_skip= n_rows - (is_top_bound? 0:1);
    }
    else
    {
      /* 
        "ROWS n FOLLOWING" is already at the first row in the next partition.
        Move it to be n_rows ahead.
      */
      n_rows_to_skip= 0;

      if ((rownum != 0) && (!is_top_bound || n_rows))
      {
        // We are positioned at the first row in the partition anyway
        //cursor.restore_cur_row();
        if (is_top_bound) // this is frame top endpoint
          item->remove();
        else
          item->add();
      }
      /* 
        Note: i_end=-1 when this is a top-endpoint "CURRENT ROW" which is
              implemented as "ROWS 0 FOLLOWING".
      */
      longlong i_end= n_rows + ((rownum==0)?1:0)- is_top_bound;
      for (longlong i= 0; i < i_end; i++)
      {
        if (next_row_intern(item))
          break;
      }
      if (i_end == -1)
      {
        if (!cursor.get_next())
          bound_tracker.check_if_next_group();
      }
    }
  }
  
  void next_row(Item_sum* item)
  {
    if (n_rows_to_skip)
    {
      n_rows_to_skip--;
      return;
    }
    if (at_partition_end)
      return;
    next_row_intern(item);
  }

private:
  bool next_row_intern(Item_sum *item)
  {
    if (!cursor_eof)
    {
      if (!(cursor_eof= (0 != cursor.get_next())))
      {
        bool new_group= is_preceding? false: bound_tracker.check_if_next_group();
        if (at_partition_start || !new_group)
        {
          if (is_top_bound) // this is frame start endpoint
            item->remove();
          else
            item->add();

          at_partition_start= false;
          return false; /* Action done */
        }
        else
        {
          at_partition_end= true;
          return true;
        }
      }
    }
    return true; /* Action not done */
  }
};


/* CURRENT ROW is the same as "ROWS 0 FOLLOWING" */
class Frame_current_row : public Frame_n_rows
{
public:
  Frame_current_row(bool is_top_bound_arg) :
    Frame_n_rows(is_top_bound_arg, false /*is_preceding*/, ha_rows(0))
  {}
};


Frame_cursor *get_frame_cursor(Window_frame *frame, bool is_top_bound)
{
  // TODO-cvicentiu When a frame is not specified, which is the frame type
  // that we will use?
  // Postgres uses RANGE BETWEEN UNBOUNDED PRECEDING AND CURRENT ROW.
  // For now we use UNBOUNDED FOLLOWING and UNBOUNDED PRECEDING.
  if (!frame)
  {
    if (is_top_bound)
      return new Frame_unbounded_preceding;
    else
      return new Frame_unbounded_following;
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
        return new Frame_unbounded_preceding;
      else
        return new Frame_unbounded_following;
    }

    if (frame->units == Window_frame::UNITS_ROWS)
    {
      longlong n_rows= bound->offset->val_int();
      return new Frame_n_rows(is_top_bound, is_preceding, n_rows);
    }
    else
    {
      // todo: Frame_range_n_rows here .
      DBUG_ASSERT(0);
    }
  }

  if (bound->precedence_type == Window_frame_bound::CURRENT)
  {
    if (frame->units == Window_frame::UNITS_ROWS)
      return new Frame_current_row(is_top_bound);
    else
    {
      if (is_top_bound)
        return new Frame_range_current_row_top;
      else
        return new Frame_range_current_row_bottom;
    }
  }
  return NULL; 
}


/*
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

bool compute_window_func_with_frames(Item_window_func *item_win,
                                     TABLE *tbl, READ_RECORD *info)
{
  THD *thd= current_thd;
  int err= 0;
  Frame_cursor *top_bound;
  Frame_cursor *bottom_bound;

  Item_sum *sum_func= item_win->window_func;
  /* This algorithm doesn't support DISTINCT aggregator */
  sum_func->set_aggregator(Aggregator::SIMPLE_AGGREGATOR);
  
  Window_frame *window_frame= item_win->window_spec->window_frame;
  top_bound= get_frame_cursor(window_frame, true);
  bottom_bound= get_frame_cursor(window_frame, false);
    
  top_bound->init(thd, info, &item_win->window_spec->partition_list,
                  &item_win->window_spec->order_list);
  bottom_bound->init(thd, info, &item_win->window_spec->partition_list,
                     &item_win->window_spec->order_list);

  bool is_error= false;
  longlong rownum= 0;
  uchar *rowid_buf= (uchar*) my_malloc(tbl->file->ref_length, MYF(0));

  while (true)
  {
    /* Move the current_row */
    if ((err=info->read_record(info)))
    {
      break; /* End of file */
    }
    bool partition_changed= (item_win->check_partition_bound() > -1)? true:
                                                                     false;
    tbl->file->position(tbl->record[0]);
    memcpy(rowid_buf, tbl->file->ref, tbl->file->ref_length);

    /* Adjust partition bounds */

    if (partition_changed || (rownum == 0))
    {
      /* Start the first partition */
      sum_func->clear();
      bottom_bound->pre_next_partition(rownum, sum_func);
      top_bound->pre_next_partition(rownum, sum_func);
      /*
        We move bottom_bound first, because we want rows to be added into the
        aggregate before top_bound attempts to remove them.
      */
      bottom_bound->next_partition(rownum, sum_func);
      top_bound->next_partition(rownum, sum_func);
    }
    else
    {
      bottom_bound->pre_next_row(sum_func);
      top_bound->pre_next_row(sum_func);

      /* These can write into tbl->record[0] */
      bottom_bound->next_row(sum_func);
      top_bound->next_row(sum_func);
    }
    rownum++;

    /*
      The bounds may have made tbl->record[0] to point to some record other
      than current_row. This applies to tbl->file's internal state, too.
      Fix this by reading the current row again.
    */
    tbl->file->ha_rnd_pos(tbl->record[0], rowid_buf);
    store_record(tbl,record[1]);
    item_win->save_in_field(item_win->result_field, true);
    err= tbl->file->ha_update_row(tbl->record[1], tbl->record[0]);
    if (err && err != HA_ERR_RECORD_IS_THE_SAME)
    {
      is_error= true;
      break;
    }
  }

  my_free(rowid_buf);
  delete top_bound;
  delete bottom_bound;
  return is_error? true: false;
}


bool compute_two_pass_window_functions(Item_window_func *item_win,
                                       TABLE *table, READ_RECORD *info)
{
  /* Perform first pass. */

  // TODO-cvicentiu why not initialize the record for when we need, _in_
  // this function.
  READ_RECORD *info2= new READ_RECORD();
  int err;
  bool is_error = false;
  bool first_row= true;
  clone_read_record(info, info2);
  Item_sum_window_with_context *window_func= 
    static_cast<Item_sum_window_with_context *>(item_win->window_func);
  uchar *rowid_buf= (uchar*) my_malloc(table->file->ref_length, MYF(0));

  is_error= window_func->create_window_context();
  /* Unable to allocate a new context. */
  if (is_error)
    return true;

  Window_context *context = window_func->get_window_context();
  /*
     The two pass algorithm is as follows:
     We have a sorted table according to the partition and order by clauses.
     1. Scan through the table till we reach a partition boundary.
     2. For each row that we scan, add it to the context.
     3. Once the partition boundary is met, do a second scan through the
     current partition and use the context information to compute the value for
     the window function for that partition.
     4. Reset the context.
     5. Repeat from 1 till end of table.
  */

  bool done = false;
  longlong rows_in_current_partition = 0;
  // TODO handle end of table updating.
  while (!done)
  {

    if ((err= info->read_record(info)))
    {
      done = true;
    }

    bool partition_changed= (done || item_win->check_partition_bound() > -1) ?
                              true : false;
    // The first time we always have a partition changed. Ignore it.
    if (first_row)
    {
      partition_changed= false;
      first_row= false;
    }

    if (partition_changed)
    {
      /*
         We are now looking at the first row for the next partition, or at the
         end of the table. Either way, we must remember this position for when
         we finish doing the second pass.
      */
      table->file->position(table->record[0]);
      memcpy(rowid_buf, table->file->ref, table->file->ref_length);

      for (longlong row_number = 0; row_number < rows_in_current_partition;
          row_number++)
      {
        if ((err= info2->read_record(info2)))
        {
          is_error= true;
          break;
        }
        window_func->add();
        // Save the window function into the table.
        item_win->save_in_field(item_win->result_field, true);
        err= table->file->ha_update_row(table->record[1], table->record[0]);
        if (err && err != HA_ERR_RECORD_IS_THE_SAME)
        {
          is_error= true;
          break;
        }
      }

      if (is_error)
        break;

      rows_in_current_partition= 0;
      window_func->clear();
      context->reset();

      // Return to the beginning of the new partition.
      table->file->ha_rnd_pos(table->record[0], rowid_buf);
    }
    rows_in_current_partition++;
    context->add_field_to_context(item_win->result_field);
  }

  window_func->delete_window_context();
  delete info2;
  my_free(rowid_buf);
  return is_error;
}


/*
  @brief
    This function is called by JOIN::exec to compute window function values
  
  @detail
    JOIN::exec calls this after it has filled the temporary table with query
    output. The temporary table has fields to store window function values.

  @return
    false OK
    true  Error
*/

bool JOIN::process_window_functions(List<Item> *curr_fields_list)
{
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

  List<Item_window_func> window_functions;
  SQL_I_List<ORDER> largest_partition;
  SQL_I_List<ORDER> largest_order_by;
  List_iterator_fast<Item> it(*curr_fields_list);
  Item *item;

#if 0
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
  {
    while ((item= it++))
    {
      if (item->type() == Item::WINDOW_FUNC_ITEM)
      {
        Item_window_func *item_win = (Item_window_func *) item;
        item_win->force_return_blank= false;
        Window_spec *spec = item_win->window_spec;
        /*
          The sorting criteria should be 
           (spec->partition_list, spec->order_list)

          Connect the two lists for the duration of add_sorting_to_table()
          call.
        */
        DBUG_ASSERT(spec->partition_list.next[0] == NULL);
        *(spec->partition_list.next)= spec->order_list.first;

        /*
           join_tab[top_join_tab_count].table is the temp. table where join
           output was stored.
        */
        add_sorting_to_table(&join_tab[top_join_tab_count],
                             spec->partition_list.first);
        join_tab[top_join_tab_count].used_for_window_func= true;

        create_sort_index(this->thd, this, &join_tab[top_join_tab_count]);
        /* Disconnect order_list from partition_list */
        *(spec->partition_list.next)= NULL;

        /*
          Go through the sorted array and compute the window function
        */
        READ_RECORD info;
        TABLE *tbl= join_tab[top_join_tab_count].table;
        if (init_read_record(&info, thd, tbl, select, 0, 1, FALSE))
          return true;
        bool is_error= false;
        
        item_win->setup_partition_border_check(thd);
      
        Item_sum::Sumfunctype type= item_win->window_func->sum_func();
        switch (type) {
          case Item_sum::ROW_NUMBER_FUNC:
          case Item_sum::RANK_FUNC:
          case Item_sum::DENSE_RANK_FUNC:
            {
              /*
                One-pass window function computation, walk through the rows and
                assign values.
              */
              if (compute_window_func_values(item_win, tbl, &info))
                is_error= true;
              break;
            }
          case Item_sum::PERCENT_RANK_FUNC:
          case Item_sum::CUME_DIST_FUNC:
            {
              if (compute_two_pass_window_functions(item_win, tbl, &info))
                is_error= true;
              break;
            }
          case Item_sum::COUNT_FUNC:
          case Item_sum::SUM_BIT_FUNC:
          {
            /*
              Frame-aware window function computation. It does one pass, but
              uses three cursors -frame_start, current_row, and frame_end.
            */
            if (compute_window_func_with_frames(item_win, tbl, &info))
              is_error= true;
            break;
          }
          default:
            DBUG_ASSERT(0);
        }

        item_win->set_read_value_from_result_field();
        /* This calls filesort_free_buffers(): */
        end_read_record(&info);

        delete join_tab[top_join_tab_count].filesort;
        join_tab[top_join_tab_count].filesort= NULL;
        free_io_cache(tbl);

        if (is_error)
          return true;
      }
    }
  }
  return false;
}

