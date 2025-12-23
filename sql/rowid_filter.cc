/*
   Copyright (c) 2018, 2019 MariaDB

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

#include "mariadb.h"
#include "table.h"
#include "sql_class.h"
#include "opt_range.h"
#include "rowid_filter.h"
#include "optimizer_defaults.h"
#include "sql_select.h"
#include "opt_trace.h"
#include "opt_hints.h"

/*
  key_next_find_cost below is the cost of finding the next possible key
  and calling handler_rowid_filter_check() to check it against the filter
*/

double Range_rowid_filter_cost_info::
lookup_cost(Rowid_filter_container_type cont_type)
{
  switch (cont_type) {
  case SORTED_ARRAY_CONTAINER:
    return log2(est_elements) * rowid_compare_cost + base_lookup_cost;
  default:
    DBUG_ASSERT_NO_ASSUME(0);
    return 0;
  }
}


/**
  @brief
    The average gain in cost per row to use the range filter with this cost
    info
*/

inline
double Range_rowid_filter_cost_info::
avg_access_and_eval_gain_per_row(Rowid_filter_container_type cont_type,
                                 double cost_of_row_fetch)
{
  return (cost_of_row_fetch + where_cost) * (1 - selectivity) -
         lookup_cost(cont_type);
}


/**
  @brief
    The average adjusted gain in cost per row of using the filter

  @param access_cost_factor the adjusted cost of access a row

  @details
    The current code to estimate the cost of a ref access is quite
    inconsistent:
    In some cases the effect of page buffers is taken into account, for others
    just the engine dependent read_time() is employed. That's why the average
    cost of one random seek might differ from 1.
    The parameter access_cost_factor can be considered as the cost of a random
    seek that is used for the given ref access. Changing the cost of a random
    seek we have to change the first coefficient in the linear formula by which
    we calculate the gain of usage the given filter for a_adj. This function
    calculates the value of a_adj.

   @note
     Currently we require that access_cost_factor should be a number between
     0.0 and 1.0
*/

inline
double Range_rowid_filter_cost_info::
avg_adjusted_gain_per_row(double access_cost_factor)
{
  DBUG_ASSERT(access_cost_factor >= 0.0 && access_cost_factor <= 1.0);
  return gain - (1 - access_cost_factor) * (1 - selectivity);
}


/**
  @brief
    Set the parameters used to choose the filter with the best adjusted gain

  @note
    This function must be called before the call of get_adjusted_gain()
    for the given filter.
*/

inline void
Range_rowid_filter_cost_info::
set_adjusted_gain_param(double access_cost_factor)
{
  gain_adj= avg_adjusted_gain_per_row(access_cost_factor);
  cross_x_adj= cost_of_building_range_filter / gain_adj;
}


/**
  @brief
    Initialize the cost info structure for a range filter

  @param cont_type  The type of the container of the range filter
  @param tab        The table for which the range filter is evaluated
  @param idx        The index used to create this range filter
*/

void Range_rowid_filter_cost_info::init(Rowid_filter_container_type cont_type,
                                        TABLE *tab, uint idx)
{
  DBUG_ASSERT(tab->opt_range_keys.is_set(idx));

  container_type= cont_type;
  table= tab;
  key_no= idx;
  est_elements= (ulonglong) table->opt_range[key_no].rows;
  cost_of_building_range_filter= build_cost(container_type);

  where_cost= tab->in_use->variables.optimizer_where_cost;
  base_lookup_cost=   (ROWID_FILTER_PER_CHECK_MODIFIER *
                       tab->file->KEY_COPY_COST);
  rowid_compare_cost= (ROWID_FILTER_PER_ELEMENT_MODIFIER *
                       tab->file->ROWID_COMPARE_COST);
  selectivity= est_elements/((double) table->stat_records());
  gain= avg_access_and_eval_gain_per_row(container_type,
                                         tab->file->ROW_LOOKUP_COST);
  if (gain > 0)
    cross_x= cost_of_building_range_filter/gain;
  else
    cross_x= cost_of_building_range_filter+1;
  abs_independent.clear_all();
}


/**
  @brief
   Return the cost of building a range filter of a certain type
*/

double
Range_rowid_filter_cost_info::build_cost(Rowid_filter_container_type cont_type)
{
  double cost;
  OPTIMIZER_COSTS *costs= &table->s->optimizer_costs;
  DBUG_ASSERT(table->opt_range_keys.is_set(key_no));

  /* Cost of fetching keys */
  cost= table->opt_range[key_no].index_only_fetch_cost(table);

  switch (cont_type) {
  case SORTED_ARRAY_CONTAINER:
    /* Add cost of filling container and cost of sorting */
    cost+= (est_elements *
            (costs->rowid_copy_cost +                      // Copying rowid
             costs->rowid_cmp_cost * log2(est_elements))); // Sort
    break;
  default:
    DBUG_ASSERT_NO_ASSUME(0);
  }

  return cost;
}


Rowid_filter_container *Range_rowid_filter_cost_info::create_container()
{
  THD *thd= table->in_use;
  uint elem_sz= table->file->ref_length;
  Rowid_filter_container *res= 0;

  switch (container_type) {
  case SORTED_ARRAY_CONTAINER:
    res= new (thd->mem_root) Rowid_filter_sorted_array((uint) est_elements,
                                                       elem_sz);
    break;
  default:
    DBUG_ASSERT_NO_ASSUME(0);
  }
  return res;
}


static int compare_range_rowid_filter_cost_info_by_a(const void *p1_,
                                                     const void *p2_)
{
  auto p1= static_cast<const Range_rowid_filter_cost_info *const *>(p1_);
  auto p2= static_cast<const Range_rowid_filter_cost_info *const *>(p2_);
  double diff= (*p2)->get_gain() - (*p1)->get_gain();
  return (diff < 0 ? -1 : (diff > 0 ? 1 : 0));
}


/**
  @brief
    Prepare the array with cost info on range filters to be used by optimizer

  @details
    The function removes the array of cost info on range filters the elements
    for those range filters that won't be ever chosen as the best filter, no
    matter what index will be used to access the table and at what step the
    table will be joined.
*/

void TABLE::prune_range_rowid_filters()
{
  /*
    For the elements of the array with cost info on range filters
    build a bit matrix of absolutely independent elements.
    Two elements are absolutely independent if they such indexes that
    there is no other index that overlaps both of them or is constraint
    correlated with both of them. Use abs_independent key maps to store
    the elements if this bit matrix.
  */

  Range_rowid_filter_cost_info **filter_ptr_1=
    range_rowid_filter_cost_info_ptr;
  for (uint i= 0;
       i < range_rowid_filter_cost_info_elems;
       i++, filter_ptr_1++)
  {
    uint key_no= (*filter_ptr_1)->key_no;
    Range_rowid_filter_cost_info **filter_ptr_2= filter_ptr_1 + 1;
    for (uint j= i+1;
         j < range_rowid_filter_cost_info_elems;
         j++, filter_ptr_2++)
    {
      key_map map_1= key_info[key_no].overlapped;
      map_1.merge(key_info[key_no].constraint_correlated);
      key_map map_2= key_info[(*filter_ptr_2)->key_no].overlapped;
      map_2.merge(key_info[(*filter_ptr_2)->key_no].constraint_correlated);
      map_1.intersect(map_2);
      if (map_1.is_clear_all())
      {
        (*filter_ptr_1)->abs_independent.set_bit((*filter_ptr_2)->key_no);
        (*filter_ptr_2)->abs_independent.set_bit(key_no);
      }
    }
  }

  /* Sort the array range_filter_cost_info by 'a' in descending order */
  my_qsort(range_rowid_filter_cost_info_ptr,
           range_rowid_filter_cost_info_elems,
           sizeof(Range_rowid_filter_cost_info *),
           (qsort_cmp) compare_range_rowid_filter_cost_info_by_a);

  /*
    For each element check whether it is created for the filter that
    can be ever chosen as the best one. If it's not the case remove
    from the array. Otherwise put it in the array in such a place
    that all already checked elements left the array are ordered by
    cross_x.
  */

  Range_rowid_filter_cost_info **cand_filter_ptr=
    range_rowid_filter_cost_info_ptr;
  for (uint i= 0;
       i < range_rowid_filter_cost_info_elems;
       i++, cand_filter_ptr++)
  {
    bool is_pruned= false;
    Range_rowid_filter_cost_info **usable_filter_ptr=
                                     range_rowid_filter_cost_info_ptr;
    key_map abs_indep;
    abs_indep.clear_all();
    for (uint j= 0; j < i; j++, usable_filter_ptr++)
    {
      if ((*cand_filter_ptr)->cross_x >= (*usable_filter_ptr)->cross_x)
      {
        if (abs_indep.is_set((*usable_filter_ptr)->key_no))
	{
          /*
            The following is true here for the element e being checked:
            There are at 2 elements e1 and e2 among already selected such that
            e1.cross_x < e.cross_x and e1.a > e.a
            and
            e2.cross_x < e_cross_x and e2.a > e.a,
            i.e. the range filters f1, f2 of both e1 and e2 always promise
            better gains then the range filter of e.
            As e1 and e2 are absolutely independent one of the range filters
            f1, f2 will be always a better choice than f1 no matter what index
            is chosen to access the table. Because of this the element e
            can be safely removed from the array.
	  */

	  is_pruned= true;
          break;
        }
        abs_indep.merge((*usable_filter_ptr)->abs_independent);
      }
      else
      {
        /*
          Move the element being checked to the proper position to have all
          elements that have been already checked to be sorted by cross_x
	*/
        Range_rowid_filter_cost_info *moved= *cand_filter_ptr;
        memmove(usable_filter_ptr+1, usable_filter_ptr,
                sizeof(Range_rowid_filter_cost_info *) * (i-j-1));
        *usable_filter_ptr= moved;
      }
    }
    if (is_pruned)
    {
      /* Remove the checked element from the array */
      memmove(cand_filter_ptr, cand_filter_ptr+1,
              sizeof(Range_rowid_filter_cost_info *) *
              (range_rowid_filter_cost_info_elems - 1 - i));
      range_rowid_filter_cost_info_elems--;
    }
  }
}


/**
   @brief
     Return maximum number of elements that a container allowed to have
 */

static ulonglong
get_max_range_rowid_filter_elems_for_table(
                                 THD *thd, TABLE *tab,
                                 Rowid_filter_container_type cont_type)
{
  switch (cont_type) {
  case SORTED_ARRAY_CONTAINER :
    return thd->variables.max_rowid_filter_size/tab->file->ref_length;
  default :
    DBUG_ASSERT_NO_ASSUME(0);
    return 0;
  }
}


/**
  @brief
    Prepare info on possible range filters used by optimizer

  @param table    The thread handler

  @details
    The function first selects the indexes of the table that potentially
    can be used for range filters and allocates an array of the objects
    of the Range_rowid_filter_cost_info type to store cost info on
    possible range filters and an array of pointers to these objects.
    The latter is created for easy sorting of the objects with cost info
    by different sort criteria. Then the function initializes the allocated
    array with cost info for each possible range filter. After this
    the function calls the method TABLE::prune_range_rowid_filters().
    The method removes the elements of the array for the filters that
    promise less gain then others remaining in the array in any situation
    and optimizes the order of the elements for faster choice of the best
    range filter.
*/

void TABLE::init_cost_info_for_usable_range_rowid_filters(THD *thd)
{
  uint key_no;
  key_map usable_range_filter_keys;
  usable_range_filter_keys.clear_all();
  key_map::Iterator it(opt_range_keys);

  if (file->ha_table_flags() & HA_NON_COMPARABLE_ROWID)
    return;                                     // Cannot create filtering

  /*
    From all indexes that can be used for range accesses select only such that
    - can be used as rowid filters                                  (1)
    - the range filter containers for them are not too large        (2)
  */
  while ((key_no= it++) != key_map::Iterator::BITMAP_END)
  {
  if (!key_can_be_used_as_rowid_filter(thd, key_no))                       // !1
      continue;
   if (opt_range[key_no].rows >
       get_max_range_rowid_filter_elems_for_table(thd, this,
                                                  SORTED_ARRAY_CONTAINER)) // !2
      continue;
    usable_range_filter_keys.set_bit(key_no);
  }

  /*
    Allocate an array of objects to store cost info for the selected filters
    and allocate an array of pointers to these objects
  */

  range_rowid_filter_cost_info_elems= usable_range_filter_keys.bits_set();
  if (!range_rowid_filter_cost_info_elems)
    return;

  range_rowid_filter_cost_info_ptr= thd->calloc<Range_rowid_filter_cost_info*>
                                      (range_rowid_filter_cost_info_elems);
  range_rowid_filter_cost_info=
    new (thd->mem_root)
      Range_rowid_filter_cost_info[range_rowid_filter_cost_info_elems];
  if (!range_rowid_filter_cost_info_ptr || !range_rowid_filter_cost_info)
  {
    range_rowid_filter_cost_info_elems= 0;
    return;
  }

  /* Fill the allocated array with cost info on the selected range filters */

  Range_rowid_filter_cost_info **curr_ptr= range_rowid_filter_cost_info_ptr;
  Range_rowid_filter_cost_info *curr_filter_cost_info=
                                                 range_rowid_filter_cost_info;

  key_map::Iterator li(usable_range_filter_keys);
  while ((key_no= li++) != key_map::Iterator::BITMAP_END)
  {
    *curr_ptr= curr_filter_cost_info;
    curr_filter_cost_info->init(SORTED_ARRAY_CONTAINER, this, key_no);
    curr_filter_cost_info->is_forced_by_hint=
        hint_key_state(thd, this, key_no, ROWID_FILTER_HINT_ENUM, false);
    curr_ptr++;
    curr_filter_cost_info++;
  }

  prune_range_rowid_filters();

  if (unlikely(thd->trace_started()))
    trace_range_rowid_filters(thd);
}


/*
  Return true if this `index` can be used as a rowid filter:
  - filter pushdown is supported by the engine for the index. If this is set
      then file->ha_table_flags() should not contain HA_NON_COMPARABLE_ROWID
  - The index is not a clustered index
  - optimizer hints ROWID_FILTER/NO_ROWID_FILTER do not forbid the use
*/

bool TABLE::key_can_be_used_as_rowid_filter(THD *thd, uint index) const
{
  return (key_info[index].index_flags &
          (HA_DO_RANGE_FILTER_PUSHDOWN | HA_CLUSTERED_INDEX)) ==
             HA_DO_RANGE_FILTER_PUSHDOWN &&
         hint_key_state(thd, this, index, ROWID_FILTER_HINT_ENUM,
                        optimizer_flag(thd, OPTIMIZER_SWITCH_USE_ROWID_FILTER));
}


/*
  Return true if a rowid filter can be applied to this `index`:
  - filter pushdown is supported by the engine for the index. If this is set
    then file->ha_table_flags() should not contain HA_NON_COMPARABLE_ROWID
  - The index is not a clustered index
*/

bool TABLE::rowid_filter_can_be_applied_to_key(uint index) const
{
  return (key_info[index].index_flags &
           (HA_DO_RANGE_FILTER_PUSHDOWN | HA_CLUSTERED_INDEX)) ==
          HA_DO_RANGE_FILTER_PUSHDOWN;
}


void TABLE::trace_range_rowid_filters(THD *thd) const
{
  DBUG_ASSERT(thd->trace_started());
  if (!range_rowid_filter_cost_info_elems)
    return;

  Range_rowid_filter_cost_info **p= range_rowid_filter_cost_info_ptr;
  Range_rowid_filter_cost_info **end= p + range_rowid_filter_cost_info_elems;

  Json_writer_object js_obj(thd);
  js_obj.add_table_name(this);
  Json_writer_array js_arr(thd, "rowid_filters");

  for (; p < end; p++)
    (*p)->trace_info(thd);
}


void Range_rowid_filter_cost_info::trace_info(THD *thd)
{
  DBUG_ASSERT(thd->trace_started());
  Json_writer_object js_obj(thd);
  js_obj.
    add("key", table->key_info[key_no].name).
    add("build_cost", cost_of_building_range_filter).
    add("rows", est_elements);
}

/**
  @brief
    Choose the best range filter for the given access of the table

  @param access_key_no      The index by which the table is accessed
  @param records            The estimated total number of key tuples with
                            this access
  @param fetch_cost_factor  The cost of fetching 'records' rows
  @param index_only_cost    The cost of fetching 'records' rows with
                            index only reads
  @param prev_records       How many index_read_calls() we expect to make
  @parma records_out        Will be updated to the minimum result rows for any
                            usable filter.
  @details
    The function looks through the array of cost info for range filters
    and chooses the element for the range filter that promise the greatest
    gain with the ref or range access of the table by access_key_no.

    The function assumes that caller has checked that the key is not a clustered
    key. See best_access_path().

  @retval  Pointer to the cost info for the range filter that promises
           the greatest gain, NULL if there is no such range filter
*/

Range_rowid_filter_cost_info *
TABLE::best_range_rowid_filter(uint access_key_no, double records,
                               double fetch_cost, double index_only_cost,
                               double prev_records, double *records_out)
{
  if (range_rowid_filter_cost_info_elems == 0 ||
      covering_keys.is_set(access_key_no))
    return 0;
  /*
    Currently we do not support usage of range filters if the table
    is accessed by the clustered primary key. It does not make sense
    if a full key is used. If the table is accessed by a partial
    clustered primary key it would, but the current InnoDB code does not
    allow it. Later this limitation may be lifted.
  */
  DBUG_ASSERT(!file->is_clustering_key(access_key_no));

  // Disallow use of range filter if the key contains partially-covered
  // columns.
  for (uint i= 0; i < key_info[access_key_no].usable_key_parts; i++)
  {
    if (key_info[access_key_no].key_part[i].field->type() == MYSQL_TYPE_BLOB)
      return 0;
  }

  Range_rowid_filter_cost_info *best_filter= 0;
  double best_filter_gain= DBL_MAX;
  bool is_forced_filter_applied= false;

  key_map no_filter_usage= key_info[access_key_no].overlapped;
  no_filter_usage.merge(key_info[access_key_no].constraint_correlated);
  no_filter_usage.set_bit(access_key_no);
  for (uint i= 0; i < range_rowid_filter_cost_info_elems ;  i++)
  {
    double new_cost, new_total_cost, new_records;
    double cost_of_accepted_rows, cost_of_rejected_rows;
    Range_rowid_filter_cost_info *filter= range_rowid_filter_cost_info_ptr[i];

    /*
      Do not use a range filter that uses an in index correlated with
      the index by which the table is accessed
    */
    if (no_filter_usage.is_set(filter->key_no))
      continue;

    new_records= records * filter->selectivity;
    set_if_smaller(*records_out, new_records);
    cost_of_accepted_rows= fetch_cost * filter->selectivity;
    cost_of_rejected_rows= index_only_cost * (1 - filter->selectivity);
    new_cost= (cost_of_accepted_rows + cost_of_rejected_rows +
               records * filter->lookup_cost());
    new_total_cost= ((new_cost + new_records *
                      in_use->variables.optimizer_where_cost) *
                     prev_records + filter->get_setup_cost());

    if (is_forced_filter_applied)
    {
      /*
        Only other forced filters can overwrite best_filter previously set
        by a forced filter
      */
      if (filter->is_forced_by_hint && new_total_cost < best_filter_gain)
      {
        best_filter_gain= new_total_cost;
        best_filter= filter;
      }
    }
    else if (new_total_cost < best_filter_gain || filter->is_forced_by_hint)
    {
      best_filter_gain= new_total_cost;
      best_filter= filter;
      is_forced_filter_applied= filter->is_forced_by_hint;
    }
  }
  return best_filter;
}


/**
  @brief
    Fill the range rowid filter performing the associated range index scan

  @details
    This function performs the range index scan associated with this
    range filter and place into the filter the rowids / primary keys
    read from key tuples when doing this scan.
  @retval
    Rowid_filter::SUCCESS          on success
    Rowid_filter::NON_FATAL_ERROR  the error which does not require transaction
                                   rollback
    Rowid_filter::FATAL_ERROR      the error which does require transaction
                                   rollback

  @note
    The function assumes that the quick select object to perform
    the index range scan has been already created.

  @note
    Currently the same table handler is used to access the joined table
    and to perform range index scan filling the filter.
    In the future two different handlers will be used for this
    purposes to facilitate a lazy building of the filter.
*/

Rowid_filter::build_return_code Range_rowid_filter::build()
{
  build_return_code rc= SUCCESS;
  handler *file= table->file;
  THD *thd= table->in_use;
  QUICK_RANGE_SELECT* quick= (QUICK_RANGE_SELECT*) select->quick;
  uint table_status_save= table->status;
  Item *pushed_idx_cond_save= file->pushed_idx_cond;
  uint pushed_idx_cond_keyno_save= file->pushed_idx_cond_keyno;
  bool in_range_check_pushed_down_save= file->in_range_check_pushed_down;
  int org_keyread;

  table->status= 0;
  file->pushed_idx_cond= 0;
  file->pushed_idx_cond_keyno= MAX_KEY;
  file->in_range_check_pushed_down= false;

  /* We're going to just read rowids / clustered primary keys */
  table->prepare_for_position();

  org_keyread= file->ha_end_active_keyread();
  file->ha_start_keyread(quick->index);

  if (quick->init() || quick->reset())
    rc= FATAL_ERROR;
  else
  {
    for (;;)
    {
      int quick_get_next_result= quick->get_next();
      if (thd->check_killed())
      {
        rc= FATAL_ERROR;
        break;
      }
      if (quick_get_next_result != 0)
      {
        rc= (quick_get_next_result == HA_ERR_END_OF_FILE ? SUCCESS
                                                        : FATAL_ERROR);
        /*
          The error state has been set by file->print_error(res, MYF(0)) call
          inside quick->get_next() call, in Mrr_simple_index_reader::get_next()
        */
        DBUG_ASSERT(rc == SUCCESS || thd->is_error());
        break;
      }
      file->position(quick->record);
      if (container->add(NULL, (char *) file->ref))
      {
        rc= NON_FATAL_ERROR;
        break;
      }
    }
  }

  quick->range_end();
  file->ha_end_keyread();
  file->ha_restart_keyread(org_keyread);

  table->status= table_status_save;
  file->pushed_idx_cond= pushed_idx_cond_save;
  file->pushed_idx_cond_keyno= pushed_idx_cond_keyno_save;
  file->in_range_check_pushed_down= in_range_check_pushed_down_save;

  tracker->set_container_elements_count(container->elements());
  tracker->report_container_buff_size(file->ref_length);

  if (rc != SUCCESS)
    return rc;

  container->sort(refpos_order_cmp, (void *) file);
  table->file->rowid_filter_is_active= true;
  return rc;
}


/**
  @brief
    Binary search in the sorted array of a rowid filter

  @param ctxt   context of the search
  @param elem   rowid / primary key to look for

  @details
    The function looks for the rowid / primary key ' elem' in this container
    assuming that ctxt contains a pointer to the TABLE structure created
    for the table to whose row elem refers to.

  @retval
    true    elem is found in the container
    false   otherwise
*/

bool Rowid_filter_sorted_array::check(void *ctxt, char *elem)
{
  handler *file= ((TABLE *) ctxt)->file;
  int l= 0;
  int r= refpos_container.elements()-1;
  while (l <= r)
  {
    int m= (l + r) / 2;
    int cmp= refpos_order_cmp((void *) file,
                              refpos_container.get_pos(m), elem);
    if (cmp == 0)
      return true;
    if (cmp < 0)
      l= m + 1;
    else
      r= m-1;
  }
  return false;
}


Range_rowid_filter::~Range_rowid_filter()
{
  delete container;
  container= 0;
  delete select;
  select= 0;
}
