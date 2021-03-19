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

#ifndef ROWID_FILTER_INCLUDED
#define ROWID_FILTER_INCLUDED


#include "mariadb.h"
#include "sql_array.h"

/*

  What rowid / primary filters are
  --------------------------------

  Consider a join query Q of the form
    SELECT * FROM T1, ... , Tk WHERE P.

  For any of the table reference Ti(Q) from the from clause of Q different
  rowid / primary key filters (pk-filters for short) can be built.
  A pk-filter F built for Ti(Q) is a set of rowids / primary keys of Ti
  F= {pk1,...,pkN} such that for any row r=r1||...||rk from the result set of Q
  ri's rowid / primary key pk(ri) is contained in F.

  When pk-filters are useful
  --------------------------

  If building a pk-filter F for Ti(Q )is not too costly and its cardinality #F
  is much less than the cardinality of T - #T then using the pk-filter when
  executing Q might be quite beneficial.

  Let r be a random row from Ti. Let s(F) be the probability that pk(r)
  belongs to F. Let BC(F) be the cost of building F.

  Suppose that the optimizer has chosen for Q a plan with this join order
  T1 => ... Tk and that the table Ti is accessed by a ref access using index I.
  Let K = {k1,...,kM} be the set of all rowid/primary keys values used to access
  rows of Ti when looking for matches in this table.to join Ti by index I.

  Let's assume that two set sets K and F are uncorrelated.  With this assumption
  if before accessing data from Ti by the rowid / primary key k we first
  check whether k is in F then we can expect saving on M*(1-s(S)) accesses of
  data rows from Ti. If we can guarantee that test whether k is in F is
  relatively cheap then we can gain a lot assuming that BC(F) is much less
  then the cost of fetching M*(1-s(S)) records from Ti and following
  evaluation of conditions pushed into Ti.

  Making pk-filter test cheap
  ---------------------------

  If the search structure to test whether an element is in F can be fully
  placed in RAM then this test is expected to be be much cheaper than a random
  access of a record from Ti. We'll consider two search structures for
  pk-filters: ordered array and bloom filter. Ordered array is easy to
  implement, but it's space consuming. If a filter contains primary keys
  then at least space for each primary key from the filter must be allocated
  in the search structure. On a the opposite a bloom filter requires a
  fixed number of bits and this number does not depend on the cardinality
  of the pk-filter (10 bits per element will serve pk-filter of any size).

*/

/*

  How and when the optimizer builds and uses range rowid filters
  --------------------------------------------------------------

  1. In make_join_statistics()
       for each join table s
         after the call of get_quick_record_count()
           the TABLE::method init_cost_info_for_usable_range_rowid_filters()
           is called
           The method build an array of Range_rowid_filter_cost_info elements
           containing the cost info on possible range filters for s->table.
           The array is optimized for further usage.

  2. For each partial join order when the optimizer considers joining
     table s to this partial join
       In the function best_access_path()
       a. When evaluating a ref access r by index idx to join s
          the optimizer estimates the effect of usage of each possible
          range filter f and chooses one with the best gain. The gain
          is taken into account when the cost of thr ref access r is
          calculated. If it turns out that this is the best ref access
          to join s then the info about the chosen filter together
          with the info on r is remembered in the corresponding element
          of the array of POSITION structures.
          [We evaluate every pair (ref access, range_filter) rather then
           every pair (best ref access, range filter) because if the index
           ref_idx used for ref access r correlates with the index rf_idx
           used  by the filter f then the pair (r,f) is not evaluated
           at all as we don't know how to estimate the effect of correlation
           between ref_idx and rf_idx.]
       b. When evaluating the best range access to join table s the
          optimizer estimates the effect of usage of each possible
          range filter f and chooses one with the best gain.
          [Here we should have evaluated every pair (range access,
           range filter) as well, but it's not done yet.]

  3. When the cheapest execution plan has been chosen and after the
     call of JOIN::get_best_combination()
       The method JOIN::make_range_rowid_filters() is called
       For each range rowid filter used in the chosen execution plan
       the method creates a quick select object to be able to perform
       index range scan to fill the filter at the execution stage.
       The method also creates Range_rowid_filter objects that are
       used at the execution stage.

  4. Just before the execution stage
       The method JOIN::init_range_rowid_filters() is called.
       For each join table s that is to be accessed with usage of a range
       filter the method allocates containers for the range filter and
       it lets the engine know that the filter will be used when
       accessing s.

  5. At the execution stage
       In the function sub_select() just before the first access of a join
       table s employing a range filter
         The method JOIN_TAB::build_range_rowid_filter_if_needed() is called
         The method fills the filter using the quick select created by
         JOIN::make_range_rowid_filters().

  6. The accessed key tuples are checked against the filter within the engine
     using the info pushed into it.

*/

struct TABLE;
class SQL_SELECT;
class Rowid_filter_container;
class Range_rowid_filter_cost_info;

/* Cost to write rowid into array */
#define ARRAY_WRITE_COST      0.005
/* Factor used to calculate cost of sorting rowids in array */
#define ARRAY_SORT_C          0.01
/* Cost to evaluate condition */
#define COST_COND_EVAL  0.2

typedef enum
{
  SORTED_ARRAY_CONTAINER,
  BLOOM_FILTER_CONTAINER      // Not used yet
} Rowid_filter_container_type;

/**
  @class Rowid_filter_container

  The interface for different types of containers to store info on the set
  of rowids / primary keys that defines a pk-filter.

  There will be two implementations of this abstract class.
  - sorted array
  - bloom filter
*/

class Rowid_filter_container : public Sql_alloc
{
public:

  virtual Rowid_filter_container_type get_type() = 0;

  /* Allocate memory for the container */
  virtual bool alloc() = 0;

  /*
    @brief Add info on a rowid / primary to the container
    @param ctxt   The context info (opaque)
    @param elem   The rowid / primary key to be added to the container
    @retval       true if elem is successfully added
  */
  virtual bool add(void *ctxt, char *elem) = 0;

  /*
    @brief Check whether a rowid / primary key is in container
    @param ctxt   The context info (opaque)
    @param elem   The rowid / primary key to be checked against the container
    @retval       False if elem is definitely not in the container
  */
  virtual bool check(void *ctxt, char *elem) = 0;

  virtual ~Rowid_filter_container() {}
};


/**
  @class Rowid_filter

  The interface for different types of pk-filters

  Currently we support only range pk filters.
*/

class Rowid_filter : public Sql_alloc
{
protected:

  /* The container to store info the set of elements in the filter */
  Rowid_filter_container *container;

  Rowid_filter_tracker *tracker;

public:
  Rowid_filter(Rowid_filter_container *container_arg)
    : container(container_arg) {}

  /*
    Build the filter :
    fill it with info on the set of elements placed there
  */
  virtual bool build() = 0;

  /*
    Check whether an element is in the filter.
    Returns false is the elements is definitely not in the filter.
  */
  virtual bool check(char *elem) = 0;

  virtual ~Rowid_filter() {}

  Rowid_filter_container *get_container() { return container; }

  void set_tracker(Rowid_filter_tracker *track_arg) { tracker= track_arg; }
  Rowid_filter_tracker *get_tracker() { return tracker; }
};


/**
  @class Rowid_filter_container

  The implementation of the Rowid_interface used for pk-filters
  that are filled when performing range index scans.
*/

class Range_rowid_filter: public Rowid_filter
{
  /* The table for which the rowid filter is built */
  TABLE *table;
  /* The select to perform the range scan to fill the filter */
  SQL_SELECT *select;
  /* The cost info on the filter (used for EXPLAIN/ANALYZE) */
  Range_rowid_filter_cost_info *cost_info;

public:
  Range_rowid_filter(TABLE *tab,
                     Range_rowid_filter_cost_info *cost_arg,
                     Rowid_filter_container *container_arg,
                     SQL_SELECT *sel)
    : Rowid_filter(container_arg), table(tab), select(sel), cost_info(cost_arg)
  {}

  ~Range_rowid_filter();

  bool build() { return fill(); }

  bool check(char *elem)
  {
    bool was_checked= container->check(table, elem);
    tracker->increment_checked_elements_count(was_checked);
    return was_checked;
  }

  bool fill();

  SQL_SELECT *get_select() { return select; }
};


/**
  @class Refpos_container_sorted_array

  The wrapper class over Dynamic_array<char> to facilitate operations over
  array of elements of the type char[N] where N is the same for all elements
*/

class Refpos_container_sorted_array : public Sql_alloc
{
  /* 
    Maximum number of elements in the array
    (Now is used only at the initialization of the dynamic array)
  */
  uint max_elements;
  /* Number of bytes allocated for an element */
  uint elem_size;
  /* The dynamic array over which the wrapper is built */
  Dynamic_array<char> *array;

public:

 Refpos_container_sorted_array(uint max_elems, uint elem_sz)
    :  max_elements(max_elems), elem_size(elem_sz), array(0) {}

  ~Refpos_container_sorted_array()
  {
    delete array;
    array= 0;
  }

  bool alloc()
  {
    array= new Dynamic_array<char> (PSI_INSTRUMENT_MEM,
                                    elem_size * max_elements,
                                    elem_size * max_elements/sizeof(char) + 1);
    return array == NULL;
  }

  bool add(char *elem)
  {
    for (uint i= 0; i < elem_size; i++)
    {
      if (array->append(elem[i]))
	return true;
    }
    return false;
  }

  char *get_pos(uint n)
  {
    return array->get_pos(n * elem_size);
  }

  uint elements() { return (uint) (array->elements() / elem_size); }

  void sort (int (*cmp) (void *ctxt, const void *el1, const void *el2),
                         void *cmp_arg)
  {
    my_qsort2(array->front(), array->elements()/elem_size,
              elem_size, (qsort2_cmp) cmp, cmp_arg);
  }
};


/**
  @class Rowid_filter_sorted_array

  The implementation of the Rowid_filter_container interface as
  a sorted array container of rowids / primary keys
*/

class Rowid_filter_sorted_array: public Rowid_filter_container
{
  /* The dynamic array to store rowids / primary keys */
  Refpos_container_sorted_array refpos_container;
  /* Initially false, becomes true after the first call of (check() */
  bool is_checked;

public:
  Rowid_filter_sorted_array(uint elems, uint elem_size)
    : refpos_container(elems, elem_size), is_checked(false) {}

  Rowid_filter_container_type get_type()
  { return SORTED_ARRAY_CONTAINER; }

  bool alloc() { return refpos_container.alloc(); }

  bool add(void *ctxt, char *elem) { return refpos_container.add(elem); }

  bool check(void *ctxt, char *elem);
};

/**
  @class Range_rowid_filter_cost_info

  An objects of this class is created for each potentially usable
  range filter. It contains the info that allows to figure out
  whether usage of the range filter promises some gain.
*/

class Range_rowid_filter_cost_info : public Sql_alloc
{
  /* The table for which the range filter is to be built (if needed) */
  TABLE *table;
  /* Estimated number of elements in the filter */
  ulonglong est_elements;
  /* The cost of building the range filter */
  double b;
  /*
     a*N-b yields the gain of the filter
     for N key tuples of the index key_no
  */
  double a;
  /* The value of N where the gain is  0 */
  double cross_x;
  /* Used for pruning of the potential range filters */
  key_map abs_independent;

  /*
    These two parameters are used to choose the best range filter
    in the function TABLE::best_range_rowid_filter_for_partial_join
  */
  double a_adj;
  double cross_x_adj;

public:
  /* The type of the container of the range filter */
  Rowid_filter_container_type container_type;
  /* The index whose range scan would be used to build the range filter */
  uint key_no;
  /* The selectivity of the range filter */
  double selectivity;

  Range_rowid_filter_cost_info() : table(0), key_no(0) {}

  void init(Rowid_filter_container_type cont_type,
            TABLE *tab, uint key_no);

  double build_cost(Rowid_filter_container_type container_type);

  inline double lookup_cost(Rowid_filter_container_type cont_type);

  inline double
  avg_access_and_eval_gain_per_row(Rowid_filter_container_type cont_type);

  inline double avg_adjusted_gain_per_row(double access_cost_factor);

  inline void set_adjusted_gain_param(double access_cost_factor);

  /* Get the gain that usage of filter promises for r key tuples */
  inline double get_gain(double r)
  {
    return r * a - b;
  }

  /* Get the adjusted gain that usage of filter promises for r key tuples */
  inline double get_adjusted_gain(double r)
  {
    return r * a_adj - b;
  }

  /*
    The gain promised by usage of the filter for r key tuples
    due to less condition evaluations
  */
  inline double get_cmp_gain(double r)
  {
    return r * (1 - selectivity) / TIME_FOR_COMPARE;
  }

  Rowid_filter_container *create_container();

  double get_a() { return a; }

  void trace_info(THD *thd);

  friend
  void TABLE::prune_range_rowid_filters();

  friend
  void TABLE::init_cost_info_for_usable_range_rowid_filters(THD *thd);

  friend
  Range_rowid_filter_cost_info *
  TABLE::best_range_rowid_filter_for_partial_join(uint access_key_no,
                                                  double records,
                                                  double access_cost_factor);
};

#endif /* ROWID_FILTER_INCLUDED */
