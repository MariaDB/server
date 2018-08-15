#ifndef ROWID_FILTER_INCLUDED
#define ROWID_FILTER_INCLUDED

/**
 It makes sense to apply filters for a certain join order when the following
 inequality holds:

 #T + c4*#T > #T*sel(Fi) + c4*#T*sel(Fi) +
              I/O(Fi) + c1*#(Fi) + c2*#(Fi)*log(#(Fi)) +
              c3*#T (1),

 where #T         - the fanout of the partial join
       Fi         - a filter for the index with the number i in 
                    the key_map of available indexes for this table
       sel(Fi)    - the selectivity of the index with the number 
                    i
       c4*#T,
       c4*#T*sel(Fi) - a cost to apply available predicates
       c4         - a constant to apply available predicates
       I/O(Fi)    - a cost of the I/O accesses to Fi
       #(Fi)      - a number of estimated records that range
                    access would use
       c1*#(Fi)   - a cost to write in Fi
       c1         - a constant to write one element in Fi
       c2*#(Fi)*log(#(Fi)) - a cost to sort in Fi
       c2         - a sorting constant
       c3*(#T)    - a cost to look-up into a current partial join
       c3         - a constant to look-up into Fi

  Let's set a new variable FBCi (filter building cost for the filter with
  index i):

  FBCi = I/O(Fi) + c1*#(Fi) + c2*#(Fi)*log(#(Fi))

  It can be seen that FBCi doesn't depend on #T.

  So using this variable (1) can be rewritten:

  #T + c4*#T > #T*sel(Fi) + c4*#T*sel(Fi) +
               FBCi +
               c3*#T

  To get a possible cost improvement when a filter is used right part
  of the (1) inequality should be deducted from the left part.
  Denote it as G(#T):

  G(#T)= #T + c4*#T - (#T*sel(Fi) + c4*#T*sel(Fi) + FBCi + c3*#T) (2)

  On the prepare stage when filters are created #T value isn't known.

  To find out what filter is the best among available one for the table
  (what filter gives the biggest gain) a knowledge about linear functions
  can be used. Consider filter gain as a linear function:

  Gi(#T)= ai*#T + bi (3)

  where ai= 1+c4-c3-sel(Fi)*(1+c4),
        bi= -FBCi

  Filter gain can be interpreted as an ordinate, #T as abscissa.

  So the aim is to find the linear function that has the biggest ordinate value
  for each positive abscissa (because #T can't be negative) comparing with
  the other available functions.

  Consider two filters Fi, Fj or linear functions with a positive slope.
  To find out which linear function is better let's find their intersection
  point coordinates.

  Gi(#T0)= Gj(#T0) (using (2))=>
  #T0= (bj - bi)/(ai - aj) (using (3))
  =>
  #T0= (BCFj-BCFi)/((sel(Fj)-sel(Fi))*(1+c4))

  If put #T0 value into the (3) formula G(#T0) can be easily found.

  It can be seen that if two linear functions intersect in II, III or IV
  quadrants the linear function with a bigger slope value will always
  be better.

  If two functions intersect in the I quadrant for #T1 < #T0 a function
  with a smaller slope value will give a better gain and when #T1 > #T0
  function with a bigger slope will give better gain.

  for each #T1 > #T0  if (ai > aj) => (Gi(#T1) >= Gj(#T1))
           #T1 <= #T0 if (ai > aj) => (Gi(#T1) <= Gj(#T1))

  So both linear functions should be saved.

  Interesting cases:

  1. For Fi,Fj filters ai=aj. 

    In this case intercepts bi and bj should be compared.
    The filter with the biggest intercept will give a better result.

  2. Only one filter remains after the calculations and for some join order
     it is equal to the index that is used to access table. Therefore, this
     filter can't be used.
     
     In this case the gain is computed for every filter that can be constructed
     for this table.

  After information about filters is computed for each partial join order
  it is checked if the filter can be applied to the current table.
  If it gives a cost improvement it is saved as the best plan for this
  partial join.
*/

#include "mariadb.h"
#include "sql_class.h"
#include "table.h"

/* Cost to write into filter */
#define COST_WRITE      0.01
/* Weight factor for filter sorting */
#define CNST_SORT       0.01
/* Cost to evaluate condition */
#define COST_COND_EVAL  0.2

class QUICK_RANGE_SELECT;

class Range_filter_cost_info : public Sql_alloc
{
public:
  TABLE *table;
  uint key_no;
  double cardinality;
  double b;                         // intercept of the linear function
  double a;                         // slope of the linear function
  double selectivity;
  double intersect_x_axis_abcissa;

  /**
    Filter cost functions
  */
  /* Cost to lookup into filter */
  inline double lookup_cost()
  {
    return log(cardinality)*0.01;
  }

  /* IO accesses cost to access filter */
  inline double filter_io_cost()
  { return table->quick_key_io[key_no]; }

  /* Cost to write elements in filter */
  inline double filter_write_cost()
  { return COST_WRITE*cardinality; }

  /* Cost to sort elements in filter */
  inline double filter_sort_cost()
  { 
    return CNST_SORT*cardinality*log(cardinality);
  }
  /* End of filter cost functions */

  Range_filter_cost_info() : table(0), key_no(0) {}

  void init(TABLE *tab, uint key_numb);

  inline double get_intersect_x(Range_filter_cost_info *filter)
  {
    if (a == filter->a)
      return DBL_MAX;
    return (b - filter->b)/(a - filter->a);
  }
  inline double get_intersect_y(double intersect_x)
  {
    if (intersect_x == DBL_MAX)
      return DBL_MAX;
    return intersect_x*a - b;
  }

  /**
    Get a gain that a usage of filter in some partial join order
    with the cardinaly card gives
  */
  inline double get_filter_gain(double card)
  {  return card*a - b;  }
};

#endif /* ROWID_FILTER_INCLUDED */
