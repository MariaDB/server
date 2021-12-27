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
#include "sql_select.h"
#include "opt_trace.h"


inline
double Range_rowid_filter_cost_info::lookup_cost(
                                     Rowid_filter_container_type cont_type)
{
  switch (cont_type) {
  case SORTED_ARRAY_CONTAINER:
    return log(est_elements)*0.01;
  default:
    DBUG_ASSERT(0);
    return 0;
  }
}


/**
  @brief
    The average gain in cost per row to use the range filter with this cost info
*/

inline
double Range_rowid_filter_cost_info::avg_access_and_eval_gain_per_row(
                                     Rowid_filter_container_type cont_type)
{
  return (1+1.0/TIME_FOR_COMPARE) * (1 - selectivity) -
         lookup_cost(cont_type);
}


/**
  @brief
    The average adjusted gain in cost per row of using the filter

  @param access_cost_factor the adjusted cost of access a row

  @details
    The current code to estimate the cost of a ref access is quite inconsistent:
    in some cases the effect of page buffers is taken into account, for others
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
double Range_rowid_filter_cost_info::avg_adjusted_gain_per_row(
				         double access_cost_factor)
{
  return a - (1 - access_cost_factor) * (1 - selectivity);
}


/**
  @brief
    Set the parameters used to choose the filter with the best adjusted gain

  @note
    This function must be called before the call of get_adjusted_gain()
    for the given filter.
*/

inline void
Range_rowid_filter_cost_info::set_adjusted_gain_param(double access_cost_factor)
{
  a_adj= avg_adjusted_gain_per_row(access_cost_factor);
  cross_x_adj= b / a_adj;
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
  b= build_cost(container_type);
  selectivity= est_elements/((double) table->stat_records());
  a= avg_access_and_eval_gain_per_row(container_type);
  if (a > 0)
    cross_x= b/a;
  else
    cross_x= b+1;
  abs_independent.clear_all();
}


/**
  @brief
   Return the cost of building a range filter of a certain type
*/

double
Range_rowid_filter_cost_info::build_cost(Rowid_filter_container_type cont_type)
{
  double cost= 0;
  DBUG_ASSERT(table->opt_range_keys.is_set(key_no));

  cost+= table->opt_range[key_no].index_only_cost;

  switch (cont_type) {

  case SORTED_ARRAY_CONTAINER:
    cost+= ARRAY_WRITE_COST * est_elements; /* cost filling the container */
    cost+= ARRAY_SORT_C * est_elements * log(est_elements); /* sorting cost */
    break;
  default:
    DBUG_ASSERT(0);
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
    DBUG_ASSERT(0);
  }
  return res;
}


static
int compare_range_rowid_filter_cost_info_by_a(
                        Range_rowid_filter_cost_info **filter_ptr_1,
                        Range_rowid_filter_cost_info **filter_ptr_2)
{
  double diff= (*filter_ptr_2)->get_a() - (*filter_ptr_1)->get_a();
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

  Range_rowid_filter_cost_info **filter_ptr_1= range_rowid_filter_cost_info_ptr;
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
    DBUG_ASSERT(0);
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
    - range filter pushdown is supported by the engine for them     (1)
    - they are not clustered primary                                (2)
    - the range filter containers for them are not too large        (3)
  */
  while ((key_no= it++) != key_map::Iterator::BITMAP_END)
  {
    if (!(file->index_flags(key_no, 0, 1) & HA_DO_RANGE_FILTER_PUSHDOWN))  // !1
      continue;
    if (file->is_clustering_key(key_no))                              // !2
      continue;
   if (opt_range[key_no].rows >
       get_max_range_rowid_filter_elems_for_table(thd, this,
                                                  SORTED_ARRAY_CONTAINER)) // !3
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

  range_rowid_filter_cost_info_ptr=
    (Range_rowid_filter_cost_info **)
      thd->calloc(sizeof(Range_rowid_filter_cost_info *) *
                  range_rowid_filter_cost_info_elems);
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
    curr_ptr++;
    curr_filter_cost_info++;
  }

  prune_range_rowid_filters();

  if (unlikely(thd->trace_started()))
    trace_range_rowid_filters(thd);
}


void TABLE::trace_range_rowid_filters(THD *thd) const
{
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
  Json_writer_object js_obj(thd);
  js_obj.add("key", table->key_info[key_no].name);
  js_obj.add("build_cost", b);
  js_obj.add("rows", est_elements);
}

/**
  @brief
    Choose the best range filter for the given access of the table

  @param access_key_no    The index by which the table is accessed
  @param records   The estimated total number of key tuples with this access
  @param access_cost_factor the cost of a random seek to access the table

  @details
    The function looks through the array of cost info for range filters
    and chooses the element for the range filter that promise the greatest
    gain with the the ref or range access of the table by access_key_no.
    As the array is sorted by cross_x in ascending order the function stops
    the look through as soon as it reaches the first element with
    cross_x_adj > records because the range filter for this element and the
    range filters for all remaining elements do not promise positive gains.

  @note
    It is easy to see that if cross_x[i] > cross_x[j] then
    cross_x_adj[i] > cross_x_adj[j]

  @retval  Pointer to the cost info for the range filter that promises
           the greatest gain, NULL if there is no such range filter
*/

Range_rowid_filter_cost_info *
TABLE::best_range_rowid_filter_for_partial_join(uint access_key_no,
                                                double records,
                                                double access_cost_factor)
{
  if (range_rowid_filter_cost_info_elems == 0 ||
      covering_keys.is_set(access_key_no))
    return 0;

  // Disallow use of range filter if the key contains partially-covered
  // columns.
  for (uint i= 0; i < key_info[access_key_no].usable_key_parts; i++)
  {
    if (key_info[access_key_no].key_part[i].field->type() == MYSQL_TYPE_BLOB)
      return 0;
  }

  /*
    Currently we do not support usage of range filters if the table
    is accessed by the clustered primary key. It does not make sense
    if a full key is used. If the table is accessed by a partial
    clustered primary key it would, but the current InnoDB code does not
    allow it. Later this limitation will be lifted
  */
  if (file->is_clustering_key(access_key_no))
    return 0;

  Range_rowid_filter_cost_info *best_filter= 0;
  double best_filter_gain= 0;

  key_map no_filter_usage= key_info[access_key_no].overlapped;
  no_filter_usage.merge(key_info[access_key_no].constraint_correlated);
  for (uint i= 0; i < range_rowid_filter_cost_info_elems ;  i++)
  {
    double curr_gain = 0;
    Range_rowid_filter_cost_info *filter= range_rowid_filter_cost_info_ptr[i];

    /*
      Do not use a range filter that uses an in index correlated with
      the index by which the table is accessed
    */
    if ((filter->key_no == access_key_no) ||
        no_filter_usage.is_set(filter->key_no))
      continue;

    filter->set_adjusted_gain_param(access_cost_factor);

    if (records < filter->cross_x_adj)
    {
      /* Does not make sense to look through the remaining filters */
      break;
    }

    curr_gain= filter->get_adjusted_gain(records);
    if (best_filter_gain < curr_gain)
    {
      best_filter_gain= curr_gain;
      best_filter= filter;
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
    false  on success
    true   otherwise

  @note
    The function assumes that the quick select object to perform
    the index range scan has been already created.

  @note
    Currently the same table handler is used to access the joined table
    and to perform range index scan filling the filter.
    In the future two different handlers will be used for this
    purposes to facilitate a lazy building of the filter.
*/

bool Range_rowid_filter::fill()
{
  int rc= 0;
  handler *file= table->file;
  THD *thd= table->in_use;
  QUICK_RANGE_SELECT* quick= (QUICK_RANGE_SELECT*) select->quick;

  uint table_status_save= table->status;
  Item *pushed_idx_cond_save= file->pushed_idx_cond;
  uint pushed_idx_cond_keyno_save= file->pushed_idx_cond_keyno;
  bool in_range_check_pushed_down_save= file->in_range_check_pushed_down;

  table->status= 0;
  file->pushed_idx_cond= 0;
  file->pushed_idx_cond_keyno= MAX_KEY;
  file->in_range_check_pushed_down= false;

  /* We're going to just read rowids / primary keys */
  table->prepare_for_position();

  table->file->ha_start_keyread(quick->index);

  if (quick->init() || quick->reset())
    rc= 1;

  while (!rc)
  {
    rc= quick->get_next();
    if (thd->killed)
      rc= 1;
    if (!rc)
    {
      file->position(quick->record);
      if (container->add(NULL, (char*) file->ref))
        rc= 1;
      else
        tracker->increment_container_elements_count();
    }
  }

  quick->range_end();
  table->file->ha_end_keyread();

  table->status= table_status_save;
  file->pushed_idx_cond= pushed_idx_cond_save;
  file->pushed_idx_cond_keyno= pushed_idx_cond_keyno_save;
  file->in_range_check_pushed_down= in_range_check_pushed_down_save;
  tracker->report_container_buff_size(table->file->ref_length);

  if (rc != HA_ERR_END_OF_FILE)
    return 1;
  table->file->rowid_filter_is_active= true;
  return 0;
}


/**
  @brief
    Binary search in the sorted array of a rowid filter

  @param ctxt   context of the search
  @parab elem   rowid / primary key to look for

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
  TABLE *table= (TABLE *) ctxt;
  if (!is_checked)
  {
    refpos_container.sort(refpos_order_cmp, (void *) (table->file));
    is_checked= true;
  }
  int l= 0;
  int r= refpos_container.elements()-1;
  while (l <= r)
  {
    int m= (l + r) / 2;
    int cmp= refpos_order_cmp((void *) (table->file),
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
  if (select)
  {
    if (select->quick)
    {
      delete select->quick;
      select->quick= 0;
    }
    delete select;
    select= 0;
  }
}
