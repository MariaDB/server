#ifndef ITEM_SUM_INCLUDED
#define ITEM_SUM_INCLUDED
/* Copyright (c) 2000, 2013 Oracle and/or its affiliates.
   Copyright (c) 2008, 2023, MariaDB Corporation.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1335  USA */


/* classes for sum functions */

#ifdef USE_PRAGMA_INTERFACE
#pragma interface			/* gcc class implementation */
#endif

#include <my_tree.h>
#include "sql_udf.h"                            /* udf_handler */

class Item_sum;
class Aggregator_distinct;
class Aggregator_simple;

/**
  The abstract base class for the Aggregator_* classes.
  It implements the data collection functions (setup/add/clear)
  as either pass-through to the real functionality or
  as collectors into an Unique (for distinct) structure.

  Note that update_field/reset_field are not in that
  class, because they're simply not called when
  GROUP BY/DISTINCT can be handled with help of index on grouped 
  fields (quick_group = 0);
*/

class Aggregator : public Sql_alloc
{
  friend class Item_sum;
  friend class Item_sum_sum;
  friend class Item_sum_count;
  friend class Item_sum_avg;

  /* 
    All members are protected as this class is not usable outside of an 
    Item_sum descendant.
  */
protected:
  /* the aggregate function class to act on */
  Item_sum *item_sum;

public:
  Aggregator (Item_sum *arg): item_sum(arg) {}
  virtual ~Aggregator () {}                   /* Keep gcc happy */

  enum Aggregator_type { SIMPLE_AGGREGATOR, DISTINCT_AGGREGATOR };
  virtual Aggregator_type Aggrtype() = 0;

  /**
    Called before adding the first row. 
    Allocates and sets up the internal aggregation structures used, 
    e.g. the Unique instance used to calculate distinct.
  */
  virtual bool setup(THD *) = 0;

  /**
    Called when we need to wipe out all the data from the aggregator :
    all the values acumulated and all the state.
    Cleans up the internal structures and resets them to their initial state.
  */
  virtual void clear() = 0;

  /**
    Called when there's a new value to be aggregated.
    Updates the internal state of the aggregator to reflect the new value.
  */
  virtual bool add() = 0;

  /**
    Called when there are no more data and the final value is to be retrieved.
    Finalises the state of the aggregator, so the final result can be retrieved.
  */
  virtual void endup() = 0;

  /** Decimal value of being-aggregated argument */
  virtual my_decimal *arg_val_decimal(my_decimal * value) = 0;
  /** Floating point value of being-aggregated argument */
  virtual double arg_val_real() = 0;
  /**
    NULLness of being-aggregated argument.

    @param use_null_value Optimization: to determine if the argument is NULL
    we must, in the general case, call is_null() on it, which itself might
    call val_*() on it, which might be costly. If you just have called
    arg_val*(), you can pass use_null_value=true; this way, arg_is_null()
    might avoid is_null() and instead do a cheap read of the Item's null_value
    (updated by arg_val*()).
  */
  virtual bool arg_is_null(bool use_null_value) = 0;
};


class st_select_lex;
class Window_spec;

/**
  Class Item_sum is the base class used for special expressions that SQL calls
  'set functions'. These expressions are formed with the help of aggregate
  functions such as SUM, MAX, GROUP_CONCAT etc.

 GENERAL NOTES

  A set function cannot be used in certain positions where expressions are
  accepted. There are some quite explicable restrictions for the usage of 
  set functions.

  In the query:
    SELECT AVG(b) FROM t1 WHERE SUM(b) > 20 GROUP by a
  the usage of the set function AVG(b) is legal, while the usage of SUM(b)
  is illegal. A WHERE condition must contain expressions that can be 
  evaluated for each row of the table. Yet the expression SUM(b) can be
  evaluated only for each group of rows with the same value of column a.
  In the query:
    SELECT AVG(b) FROM t1 WHERE c > 30 GROUP BY a HAVING SUM(b) > 20
  both set function expressions AVG(b) and SUM(b) are legal.

  We can say that in a query without nested selects an occurrence of a
  set function in an expression of the SELECT list or/and in the HAVING
  clause is legal, while in the WHERE clause it's illegal.

  The general rule to detect whether a set function is legal in a query with
  nested subqueries is much more complicated.

  Consider the the following query:
    SELECT t1.a FROM t1 GROUP BY t1.a
      HAVING t1.a > ALL (SELECT t2.c FROM t2 WHERE SUM(t1.b) < t2.c).
  The set function SUM(b) is used here in the WHERE clause of the subquery.
  Nevertheless it is legal since it is under the HAVING clause of the query
  to which this function relates. The expression SUM(t1.b) is evaluated
  for each group defined in the main query, not for groups of the subquery.

  The problem of finding the query where to aggregate a particular
  set function is not so simple as it seems to be.

  In the query: 
    SELECT t1.a FROM t1 GROUP BY t1.a
     HAVING t1.a > ALL(SELECT t2.c FROM t2 GROUP BY t2.c
                         HAVING SUM(t1.a) < t2.c)
  the set function can be evaluated for both outer and inner selects.
  If we evaluate SUM(t1.a) for the outer query then we get the value of t1.a
  multiplied by the cardinality of a group in table t1. In this case 
  in each correlated subquery SUM(t1.a) is used as a constant. But we also
  can evaluate SUM(t1.a) for the inner query. In this case t1.a will be a
  constant for each correlated subquery and summation is performed
  for each group of table t2.
  (Here it makes sense to remind that the query
    SELECT c FROM t GROUP BY a HAVING SUM(1) < a 
  is quite legal in our SQL).

  So depending on what query we assign the set function to we
  can get different result sets.

  The general rule to detect the query where a set function is to be
  evaluated can be formulated as follows.
  Consider a set function S(E) where E is an expression with occurrences
  of column references C1, ..., CN. Resolve these column references against
  subqueries that contain the set function S(E). Let Q be the innermost
  subquery of those subqueries. (It should be noted here that S(E)
  in no way can be evaluated in the subquery embedding the subquery Q,
  otherwise S(E) would refer to at least one unbound column reference)
  If S(E) is used in a construct of Q where set functions are allowed then
  we evaluate S(E) in Q.
  Otherwise we look for a innermost subquery containing S(E) of those where
  usage of S(E) is allowed.

  Let's demonstrate how this rule is applied to the following queries.

  1. SELECT t1.a FROM t1 GROUP BY t1.a
       HAVING t1.a > ALL(SELECT t2.b FROM t2 GROUP BY t2.b
                           HAVING t2.b > ALL(SELECT t3.c FROM t3 GROUP BY t3.c
                                                HAVING SUM(t1.a+t2.b) < t3.c))
  For this query the set function SUM(t1.a+t2.b) depends on t1.a and t2.b
  with t1.a defined in the outermost query, and t2.b defined for its
  subquery. The set function is in the HAVING clause of the subquery and can
  be evaluated in this subquery.

  2. SELECT t1.a FROM t1 GROUP BY t1.a
       HAVING t1.a > ALL(SELECT t2.b FROM t2
                           WHERE t2.b > ALL (SELECT t3.c FROM t3 GROUP BY t3.c
                                               HAVING SUM(t1.a+t2.b) < t3.c))
  Here the set function SUM(t1.a+t2.b)is in the WHERE clause of the second
  subquery - the most upper subquery where t1.a and t2.b are defined.
  If we evaluate the function in this subquery we violate the context rules.
  So we evaluate the function in the third subquery (over table t3) where it
  is used under the HAVING clause.

  3. SELECT t1.a FROM t1 GROUP BY t1.a
       HAVING t1.a > ALL(SELECT t2.b FROM t2
                           WHERE t2.b > ALL (SELECT t3.c FROM t3 
                                               WHERE SUM(t1.a+t2.b) < t3.c))
  In this query evaluation of SUM(t1.a+t2.b) is not legal neither in the second
  nor in the third subqueries. So this query is invalid.

  Mostly set functions cannot be nested. In the query
    SELECT t1.a from t1 GROUP BY t1.a HAVING AVG(SUM(t1.b)) > 20
  the expression SUM(b) is not acceptable, though it is under a HAVING clause.
  Yet it is acceptable in the query:
    SELECT t.1 FROM t1 GROUP BY t1.a HAVING SUM(t1.b) > 20.

  An argument of a set function does not have to be a reference to a table
  column as we saw it in examples above. This can be a more complex expression
    SELECT t1.a FROM t1 GROUP BY t1.a HAVING SUM(t1.b+1) > 20.
  The expression SUM(t1.b+1) has a very clear semantics in this context:
  we sum up the values of t1.b+1 where t1.b varies for all values within a
  group of rows that contain the same t1.a value.

  A set function for an outer query yields a constant within a subquery. So
  the semantics of the query
    SELECT t1.a FROM t1 GROUP BY t1.a
      HAVING t1.a IN (SELECT t2.c FROM t2 GROUP BY t2.c
                        HAVING AVG(t2.c+SUM(t1.b)) > 20)
  is still clear. For a group of the rows with the same t1.a values we
  calculate the value of SUM(t1.b). This value 's' is substituted in the
  the subquery:
    SELECT t2.c FROM t2 GROUP BY t2.c HAVING AVG(t2.c+s)
  than returns some result set.

  By the same reason the following query with a subquery 
    SELECT t1.a FROM t1 GROUP BY t1.a
      HAVING t1.a IN (SELECT t2.c FROM t2 GROUP BY t2.c
                        HAVING AVG(SUM(t1.b)) > 20)
  is also acceptable.

 IMPLEMENTATION NOTES

  Three methods were added to the class to check the constraints specified
  in the previous section. These methods utilize several new members.

  The field 'nest_level' contains the number of the level for the subquery
  containing the set function. The main SELECT is of level 0, its subqueries
  are of levels 1, the subqueries of the latter are of level 2 and so on.

  The field 'aggr_level' is to contain the nest level of the subquery
  where the set function is aggregated.

  The field 'max_arg_level' is for the maximum of the nest levels of the
  unbound column references occurred in the set function. A column reference
  is unbound  within a set function if it is not bound by any subquery
  used as a subexpression in this function. A column reference is bound by
  a subquery if it is a reference to the column by which the aggregation
  of some set function that is used in the subquery is calculated.
  For the set function used in the query
    SELECT t1.a FROM t1 GROUP BY t1.a
      HAVING t1.a > ALL(SELECT t2.b FROM t2 GROUP BY t2.b
                          HAVING t2.b > ALL(SELECT t3.c FROM t3 GROUP BY t3.c
                                              HAVING SUM(t1.a+t2.b) < t3.c))
  the value of max_arg_level is equal to 1 since t1.a is bound in the main
  query, and t2.b is bound by the first subquery whose nest level is 1.
  Obviously a set function cannot be aggregated in the subquery whose
  nest level is less than max_arg_level. (Yet it can be aggregated in the
  subqueries whose nest level is greater than max_arg_level.)
  In the query
    SELECT t.a FROM t1 HAVING AVG(t1.a+(SELECT MIN(t2.c) FROM t2))
  the value of the max_arg_level for the AVG set function is 0 since
  the reference t2.c is bound in the subquery.

  The field 'max_sum_func_level' is to contain the maximum of the
  nest levels of the set functions that are used as subexpressions of
  the arguments of the given set function, but not aggregated in any
  subquery within this set function. A nested set function s1 can be
  used within set function s0 only if s1.max_sum_func_level <
  s0.max_sum_func_level. Set function s1 is considered as nested
  for set function s0 if s1 is not calculated in any subquery
  within s0.

  A set function that is used as a subexpression in an argument of another
  set function refers to the latter via the field 'in_sum_func'.

  The condition imposed on the usage of set functions are checked when
  we traverse query subexpressions with the help of the recursive method
  fix_fields. When we apply this method to an object of the class
  Item_sum, first, on the descent, we call the method init_sum_func_check
  that initialize members used at checking. Then, on the ascent, we
  call the method check_sum_func that validates the set function usage
  and reports an error if it is illegal.
  The method register_sum_func serves to link the items for the set functions
  that are aggregated in the embedding (sub)queries. Circular chains of such
  functions are attached to the corresponding st_select_lex structures
  through the field inner_sum_func_list.

  Exploiting the fact that the members mentioned above are used in one
  recursive function we could have allocated them on the thread stack.
  Yet we don't do it now.
  
  We assume that the nesting level of subquries does not exceed 127.
  TODO: to catch queries where the limit is exceeded to make the
  code clean here.  

  @note
  The implementation takes into account the used strategy:
  - Items resolved at optimization phase return 0 from Item_sum::used_tables().
  - Items that depend on the number of join output records, but not columns of
  any particular table (like COUNT(*)), returm 0 from Item_sum::used_tables(),
  but still return false from Item_sum::const_item().
*/

class Item_sum :public Item_func_or_sum
{
  friend class Aggregator_distinct;
  friend class Aggregator_simple;

protected:
  /**
    Aggregator class instance. Not set initially. Allocated only after
    it is determined if the incoming data are already distinct.
  */
  Aggregator *aggr;

private:
  /**
    Used in making ROLLUP. Set for the ROLLUP copies of the original
    Item_sum and passed to create_tmp_field() to cause it to work
    over the temp table buffer that is referenced by
    Item_result_field::result_field.
  */
  bool force_copy_fields;

  /**
    Indicates how the aggregate function was specified by the parser :
    1 if it was written as AGGREGATE(DISTINCT),
    0 if it was AGGREGATE()
  */
  bool with_distinct;

  /* TRUE if this is aggregate function of a window function */
  bool window_func_sum_expr_flag;

public:

  bool has_force_copy_fields() const { return force_copy_fields; }
  bool has_with_distinct()     const { return with_distinct; }

  enum Sumfunctype
  { COUNT_FUNC, COUNT_DISTINCT_FUNC, SUM_FUNC, SUM_DISTINCT_FUNC, AVG_FUNC,
    AVG_DISTINCT_FUNC, MIN_FUNC, MAX_FUNC, STD_FUNC,
    VARIANCE_FUNC, SUM_BIT_FUNC, UDF_SUM_FUNC, GROUP_CONCAT_FUNC,
    ROW_NUMBER_FUNC, RANK_FUNC, DENSE_RANK_FUNC, PERCENT_RANK_FUNC,
    CUME_DIST_FUNC, NTILE_FUNC, FIRST_VALUE_FUNC, LAST_VALUE_FUNC,
    NTH_VALUE_FUNC, LEAD_FUNC, LAG_FUNC, PERCENTILE_CONT_FUNC,
    PERCENTILE_DISC_FUNC, SP_AGGREGATE_FUNC, JSON_ARRAYAGG_FUNC,
    JSON_OBJECTAGG_FUNC
  };

  Item **ref_by; /* pointer to a ref to the object used to register it */
  Item_sum *next; /* next in the circular chain of registered objects  */
  Item_sum *in_sum_func;  /* embedding set function if any */ 
  st_select_lex * aggr_sel; /* select where the function is aggregated       */ 
  int8 nest_level;        /* number of the nesting level of the set function */
  int8 aggr_level;        /* nesting level of the aggregating subquery       */
  int8 max_arg_level;     /* max level of unbound column references          */
  int8 max_sum_func_level;/* max level of aggregation for embedded functions */

  /*
    true  (the default value) means this aggregate function can be computed
          with TemporaryTableWithPartialSums algorithm (see end_update()).
    false means this aggregate function needs OrderedGroupBy algorithm (see
          end_write_group()).
  */
  bool quick_group;
  /*
    This list is used by the check for mixing non aggregated fields and
    sum functions in the ONLY_FULL_GROUP_BY_MODE. We save all outer fields
    directly or indirectly used under this function it as it's unclear
    at the moment of fixing outer field whether it's aggregated or not.
  */
  List<Item_field> outer_fields;

protected:  
  /* 
    Copy of the arguments list to hold the original set of arguments.
    Used in EXPLAIN EXTENDED instead of the current argument list because 
    the current argument list can be altered by usage of temporary tables.
  */
  Item **orig_args, *tmp_orig_args[2];
  
  static size_t ram_limitation(THD *thd);
public:
  // Methods used by ColumnStore
  Item **get_orig_args() const { return orig_args; }
public:  

  void mark_as_sum_func();
  Item_sum(THD *thd): Item_func_or_sum(thd), quick_group(1)
  {
    mark_as_sum_func();
    init_aggregator();
  }
  Item_sum(THD *thd, Item *a): Item_func_or_sum(thd, a), quick_group(1),
    orig_args(tmp_orig_args)
  {
    mark_as_sum_func();
    init_aggregator();
  }
  Item_sum(THD *thd, Item *a, Item *b): Item_func_or_sum(thd, a, b),
    quick_group(1), orig_args(tmp_orig_args)
  {
    mark_as_sum_func();
    init_aggregator();
  }
  Item_sum(THD *thd, List<Item> &list);
  //Copy constructor, need to perform subselects with temporary tables
  Item_sum(THD *thd, Item_sum *item);
  enum Type type() const override { return SUM_FUNC_ITEM; }
  virtual enum Sumfunctype sum_func () const=0;
  bool is_aggr_sum_func()
  {
    switch (sum_func()) {
    case COUNT_FUNC:
    case COUNT_DISTINCT_FUNC:
    case SUM_FUNC:
    case SUM_DISTINCT_FUNC:
    case AVG_FUNC:
    case AVG_DISTINCT_FUNC:
    case MIN_FUNC:
    case MAX_FUNC:
    case STD_FUNC:
    case VARIANCE_FUNC:
    case SUM_BIT_FUNC:
    case UDF_SUM_FUNC:
    case GROUP_CONCAT_FUNC:
    case JSON_ARRAYAGG_FUNC:
      return true;
    default:
      return false;
    }
  }
  /**
    Resets the aggregate value to its default and aggregates the current
    value of its attribute(s).
  */
  inline bool reset_and_add() 
  { 
    aggregator_clear();
    return aggregator_add();
  };

  /*
    Called when new group is started and results are being saved in
    a temporary table. Similarly to reset_and_add() it resets the 
    value to its default and aggregates the value of its 
    attribute(s), but must also store it in result_field. 
    This set of methods (result_item(), reset_field, update_field()) of
    Item_sum is used only if quick_group is not null. Otherwise
    copy_or_same() is used to obtain a copy of this item.
  */
  virtual void reset_field()=0;
  /*
    Called for each new value in the group, when temporary table is in use.
    Similar to add(), but uses temporary table field to obtain current value,
    Updated value is then saved in the field.
  */
  virtual void update_field()=0;
  bool fix_length_and_dec(THD *thd) override
  {
    set_maybe_null();
    null_value=1;
    return FALSE;
  }
  virtual Item *result_item(THD *thd, Field *field);

  void update_used_tables() override;
  COND *build_equal_items(THD *thd, COND_EQUAL *inherited,
                          bool link_item_fields,
                          COND_EQUAL **cond_equal_ref) override
  {
    /*
      Item_sum (and derivants) of the original WHERE/HAVING clauses
      should already be replaced to Item_aggregate_ref by the time when
      build_equal_items() is called. See Item::split_sum_func2().
    */
    DBUG_ASSERT(0);
    return Item::build_equal_items(thd, inherited, link_item_fields,
                                   cond_equal_ref);
  }
  bool is_null() override { return null_value; }
  /**
    make_const()
    Called if we've managed to calculate the value of this Item in
    opt_sum_query(), hence it can be considered constant at all subsequent
    steps.
  */
  void make_const () 
  { 
    used_tables_cache= 0;
    const_item_cache= true;
  }
  void reset_forced_const() { const_item_cache= false; }
  bool const_during_execution() const override { return false; }
  void print(String *str, enum_query_type query_type) override;
  void fix_num_length_and_dec();

  /**
    Mark an aggregate as having no rows.

    This function is called by the execution engine to assign 'NO ROWS
    FOUND' value to an aggregate item, when the underlying result set
    has no rows. Such value, in a general case, may be different from
    the default value of the item after 'clear()': e.g. a numeric item
    may be initialized to 0 by clear() and to NULL by
    no_rows_in_result().
  */
  void no_rows_in_result() override
  {
    set_aggregator(current_thd, with_distinct ?
                   Aggregator::DISTINCT_AGGREGATOR :
                   Aggregator::SIMPLE_AGGREGATOR);
    aggregator_clear();
  }
  virtual void make_unique() { force_copy_fields= TRUE; }
  Item *get_tmp_table_item(THD *thd) override;
  virtual Field *create_tmp_field(MEM_ROOT *root, bool group, TABLE *table);
  Field *create_tmp_field_ex(MEM_ROOT *root, TABLE *table, Tmp_field_src *src,
                             const Tmp_field_param *param) override
  {
    return create_tmp_field(root, param->group(), table);
  }
  bool collect_outer_ref_processor(void *param) override;
  bool init_sum_func_check(THD *thd);
  bool check_sum_func(THD *thd, Item **ref);
  bool register_sum_func(THD *thd, Item **ref);
  st_select_lex *depended_from() 
    { return (nest_level == aggr_level ? 0 : aggr_sel); }

  Item *get_arg(uint i) const { return args[i]; }
  Item *set_arg(uint i, THD *thd, Item *new_val);
  uint get_arg_count() const { return arg_count; }
  virtual Item **get_args() { return fixed() ? orig_args : args; }

  /* Initialization of distinct related members */
  void init_aggregator()
  {
    aggr= NULL;
    with_distinct= FALSE;
    force_copy_fields= FALSE;
  }

  /**
    Called to initialize the aggregator.
  */

  inline bool aggregator_setup(THD *thd) { return aggr->setup(thd); };

  /**
    Called to cleanup the aggregator.
  */

  inline void aggregator_clear() { aggr->clear(); }

  /**
    Called to add value to the aggregator.
  */

  inline bool aggregator_add() { return aggr->add(); };

  /* stores the declared DISTINCT flag (from the parser) */
  void set_distinct(bool distinct)
  {
    with_distinct= distinct;
    quick_group= with_distinct ? 0 : 1;
  }

  /*
    Set the type of aggregation : DISTINCT or not.

    May be called multiple times.
  */

  int set_aggregator(THD *thd, Aggregator::Aggregator_type aggregator);

  virtual void clear()= 0;
  virtual bool add()= 0;
  virtual bool setup(THD *thd) { return false; }

  virtual bool supports_removal() const { return false; }
  virtual void remove() { DBUG_ASSERT(0); }

  void cleanup() override;
  bool check_vcol_func_processor(void *arg) override;
  virtual void setup_window_func(THD *thd, Window_spec *window_spec) {}
  void mark_as_window_func_sum_expr() { window_func_sum_expr_flag= true; }
  bool is_window_func_sum_expr() { return window_func_sum_expr_flag; }
  virtual void setup_caches(THD *thd) {};
  virtual void set_partition_row_count(ulonglong count) { DBUG_ASSERT(0); }
};


class Unique;


/**
 The distinct aggregator. 
 Implements AGGFN (DISTINCT ..)
 Collects all the data into an Unique (similarly to what Item_sum
 does currently when with_distinct=true) and then (if applicable) iterates over
 the list of unique values and pumps them back into its object
*/

class Aggregator_distinct : public Aggregator
{
  friend class Item_sum_sum;

  /* 
    flag to prevent consecutive runs of endup(). Normally in endup there are 
    expensive calculations (like walking the distinct tree for example) 
    which we must do only once if there are no data changes.
    We can re-use the data for the second and subsequent val_xxx() calls.
    endup_done set to TRUE also means that the calculated values for
    the aggregate functions are correct and don't need recalculation.
  */
  bool endup_done;

  /*
    Used depending on the type of the aggregate function and the presence of
    blob columns in it:
    - For COUNT(DISTINCT) and no blob fields this points to a real temporary
      table. It's used as a hash table.
    - For AVG/SUM(DISTINCT) or COUNT(DISTINCT) with blob fields only the
      in-memory data structure of a temporary table is constructed.
      It's used by the Field classes to transform data into row format.
  */
  TABLE *table;
  
  /*
    An array of field lengths on row allocated and used only for 
    COUNT(DISTINCT) with multiple columns and no blobs. Used in 
    Aggregator_distinct::composite_key_cmp (called from Unique to compare 
    nodes
  */
  uint32 *field_lengths;

  /*
    Used in conjunction with 'table' to support the access to Field classes 
    for COUNT(DISTINCT). Needed by copy_fields()/copy_funcs().
  */
  TMP_TABLE_PARAM *tmp_table_param;
  
  /*
    If there are no blobs in the COUNT(DISTINCT) arguments, we can use a tree,
    which is faster than heap table. In that case, we still use the table
    to help get things set up, but we insert nothing in it. 
    For AVG/SUM(DISTINCT) we always use this tree (as it takes a single 
    argument) to get the distinct rows.
  */
  Unique *tree;

  /* 
    The length of the temp table row. Must be a member of the class as it
    gets passed down to simple_raw_key_cmp () as a compare function argument
    to Unique. simple_raw_key_cmp () is used as a fast comparison function 
    when the entire row can be binary compared.
  */  
  uint tree_key_length;

  /* 
    Set to true if the result is known to be always NULL.
    If set deactivates creation and usage of the temporary table (in the 
    'table' member) and the Unique instance (in the 'tree' member) as well as 
    the calculation of the final value on the first call to 
    Item_[sum|avg|count]::val_xxx(). 
  */
  bool always_null;

  /**
    When feeding back the data in endup() from Unique/temp table back to
    Item_sum::add() methods we must read the data from Unique (and not
    recalculate the functions that are given as arguments to the aggregate
    function.
    This flag is to tell the arg_*() methods to take the data from the Unique
    instead of calling the relevant val_..() method.
  */
  bool use_distinct_values;

public:
  Aggregator_distinct (Item_sum *sum) :
    Aggregator(sum), table(NULL), tmp_table_param(NULL), tree(NULL),
    always_null(false), use_distinct_values(false) {}
  virtual ~Aggregator_distinct ();
  Aggregator_type Aggrtype() { return DISTINCT_AGGREGATOR; }

  bool setup(THD *);
  void clear(); 
  bool add();
  void endup();
  virtual my_decimal *arg_val_decimal(my_decimal * value);
  virtual double arg_val_real();
  virtual bool arg_is_null(bool use_null_value);

  bool unique_walk_function(void *element);
  bool unique_walk_function_for_count(void *element);
  static int composite_key_cmp(void* arg, uchar* key1, uchar* key2);
};


/**
  The pass-through aggregator. 
  Implements AGGFN (DISTINCT ..) by knowing it gets distinct data on input. 
  So it just pumps them back to the Item_sum descendant class.
*/
class Aggregator_simple : public Aggregator
{
public:

  Aggregator_simple (Item_sum *sum) :
    Aggregator(sum) {}
  Aggregator_type Aggrtype() { return Aggregator::SIMPLE_AGGREGATOR; }

  bool setup(THD * thd) { return item_sum->setup(thd); }
  void clear() { item_sum->clear(); }
  bool add() { return item_sum->add(); }
  void endup() {};
  virtual my_decimal *arg_val_decimal(my_decimal * value);
  virtual double arg_val_real();
  virtual bool arg_is_null(bool use_null_value);
};


class Item_sum_num :public Item_sum
{
public:
  Item_sum_num(THD *thd): Item_sum(thd) {}
  Item_sum_num(THD *thd, Item *item_par):
    Item_sum(thd, item_par) {}
  Item_sum_num(THD *thd, Item *a, Item* b):
    Item_sum(thd, a, b) {}
  Item_sum_num(THD *thd, List<Item> &list):
    Item_sum(thd, list) {}
  Item_sum_num(THD *thd, Item_sum_num *item):
    Item_sum(thd, item) {}
  bool fix_fields(THD *, Item **);
};


class Item_sum_double :public Item_sum_num
{
public:
  Item_sum_double(THD *thd): Item_sum_num(thd) {}
  Item_sum_double(THD *thd, Item *item_par): Item_sum_num(thd, item_par) {}
  Item_sum_double(THD *thd, List<Item> &list): Item_sum_num(thd, list) {}
  Item_sum_double(THD *thd, Item_sum_double *item) :Item_sum_num(thd, item) {}
  longlong val_int() override
  {
    return val_int_from_real();
  }
  String *val_str(String*str) override
  {
    return val_string_from_real(str);
  }
  my_decimal *val_decimal(my_decimal *to) override
  {
    return val_decimal_from_real(to);
  }
  bool get_date(THD *thd, MYSQL_TIME *ltime, date_mode_t fuzzydate) override
  {
    return get_date_from_real(thd, ltime, fuzzydate);
  }
  const Type_handler *type_handler() const override
  { return &type_handler_double; }
};


class Item_sum_int :public Item_sum_num
{
public:
  Item_sum_int(THD *thd): Item_sum_num(thd) {}
  Item_sum_int(THD *thd, Item *item_par): Item_sum_num(thd, item_par) {}
  Item_sum_int(THD *thd, List<Item> &list): Item_sum_num(thd, list) {}
  Item_sum_int(THD *thd, Item_sum_int *item) :Item_sum_num(thd, item) {}
  double val_real() override { DBUG_ASSERT(fixed()); return (double) val_int(); }
  String *val_str(String*str) override;
  my_decimal *val_decimal(my_decimal *) override;
  bool get_date(THD *thd, MYSQL_TIME *ltime, date_mode_t fuzzydate) override
  {
    return get_date_from_int(thd, ltime, fuzzydate);
  }
  bool fix_length_and_dec(THD *thd) override
  {
    decimals=0;
    max_length=21;
    base_flags&= ~item_base_t::MAYBE_NULL;
    null_value=0;
    return FALSE; }
};


class Item_sum_sum :public Item_sum_num,
                   public Type_handler_hybrid_field_type 
{
protected:
  bool direct_added;
  bool direct_reseted_field;
  bool direct_sum_is_null;
  double direct_sum_real;
  double sum;
  my_decimal direct_sum_decimal;
  my_decimal dec_buffs[2];
  uint curr_dec_buff;
  bool fix_length_and_dec(THD *thd) override;

public:
  Item_sum_sum(THD *thd, Item *item_par, bool distinct):
    Item_sum_num(thd, item_par), direct_added(FALSE),
    direct_reseted_field(FALSE)
  {
    set_distinct(distinct);
  }
  Item_sum_sum(THD *thd, Item_sum_sum *item);
  enum Sumfunctype sum_func() const override
  { 
    return has_with_distinct() ? SUM_DISTINCT_FUNC : SUM_FUNC; 
  }
  void cleanup() override;
  void direct_add(my_decimal *add_sum_decimal);
  void direct_add(double add_sum_real, bool add_sum_is_null);
  void clear() override;
  bool add() override;
  double val_real() override;
  longlong val_int() override;
  String *val_str(String*str) override;
  my_decimal *val_decimal(my_decimal *) override;
  bool get_date(THD *thd, MYSQL_TIME *ltime, date_mode_t fuzzydate) override
  {
    return type_handler()->Item_get_date_with_warn(thd, this, ltime, fuzzydate);
  }
  const Type_handler *type_handler() const override
  { return Type_handler_hybrid_field_type::type_handler(); }
  void fix_length_and_dec_double();
  void fix_length_and_dec_decimal();
  void reset_field() override;
  void update_field() override;
  void no_rows_in_result() override {}
  LEX_CSTRING func_name_cstring() const override
  { 
    static LEX_CSTRING name_distinct= { STRING_WITH_LEN("sum(distinct ")};
    static LEX_CSTRING name_normal=   { STRING_WITH_LEN("sum(") };
    return has_with_distinct() ? name_distinct : name_normal;
  }
  Item *copy_or_same(THD* thd) override;
  void remove() override;
  Item *get_copy(THD *thd) override
  { return get_item_copy<Item_sum_sum>(thd, this); }

  bool supports_removal() const override
  {
    return true;
  }

private:
  void add_helper(bool perform_removal);
  ulonglong count;
};


class Item_sum_count :public Item_sum_int
{
  bool direct_counted;
  bool direct_reseted_field;
  longlong direct_count;
  longlong count;

  friend class Aggregator_distinct;

  void clear() override;
  bool add() override;
  void cleanup() override;
  void remove() override;

public:
  Item_sum_count(THD *thd, Item *item_par):
    Item_sum_int(thd, item_par), direct_counted(FALSE),
    direct_reseted_field(FALSE), count(0)
  {}

  /**
    Constructs an instance for COUNT(DISTINCT)

    @param list  a list of the arguments to the aggregate function

    This constructor is called by the parser only for COUNT (DISTINCT).
  */

  Item_sum_count(THD *thd, List<Item> &list):
    Item_sum_int(thd, list), direct_counted(FALSE),
    direct_reseted_field(FALSE), count(0)
  {
    set_distinct(TRUE);
  }
  Item_sum_count(THD *thd, Item_sum_count *item):
    Item_sum_int(thd, item), direct_counted(FALSE),
    direct_reseted_field(FALSE), count(item->count)
  {}
  enum Sumfunctype sum_func () const override
  { 
    return has_with_distinct() ? COUNT_DISTINCT_FUNC : COUNT_FUNC; 
  }
  void no_rows_in_result() override { count=0; }
  void make_const(longlong count_arg) 
  { 
    count=count_arg;
    Item_sum::make_const();
  }
  const Type_handler *type_handler() const override
  { return &type_handler_slonglong; }
  longlong val_int() override;
  void reset_field() override;
  void update_field() override;
  void direct_add(longlong add_count);
  LEX_CSTRING func_name_cstring() const override
  { 
    static LEX_CSTRING name_distinct= { STRING_WITH_LEN("count(distinct ")};
    static LEX_CSTRING name_normal=   { STRING_WITH_LEN("count(") };
    return has_with_distinct() ? name_distinct : name_normal;
  }
  Item *copy_or_same(THD* thd) override;
  Item *get_copy(THD *thd) override
  { return get_item_copy<Item_sum_count>(thd, this); }

  bool supports_removal() const override
  {
    return true;
  }
};


class Item_sum_avg :public Item_sum_sum
{
public:
  // TODO-cvicentiu given that Item_sum_sum now uses a counter of its own, in
  // order to implement remove(), it is possible to remove this member.
  ulonglong count;
  uint prec_increment;
  uint f_precision, f_scale, dec_bin_size;

  Item_sum_avg(THD *thd, Item *item_par, bool distinct):
    Item_sum_sum(thd, item_par, distinct), count(0)
  {}
  Item_sum_avg(THD *thd, Item_sum_avg *item)
    :Item_sum_sum(thd, item), count(item->count),
    prec_increment(item->prec_increment) {}

  void fix_length_and_dec_double();
  void fix_length_and_dec_decimal();
  bool fix_length_and_dec(THD *thd) override;
  enum Sumfunctype sum_func () const override
  {
    return has_with_distinct() ? AVG_DISTINCT_FUNC : AVG_FUNC;
  }
  void clear() override;
  bool add() override;
  void remove() override;
  double val_real() override;
  // In SPs we might force the "wrong" type with select into a declare variable
  longlong val_int() override { return val_int_from_real(); }
  my_decimal *val_decimal(my_decimal *) override;
  String *val_str(String *str) override;
  void reset_field() override;
  void update_field() override;
  Item *result_item(THD *thd, Field *field) override;
  void no_rows_in_result() override {}
  LEX_CSTRING func_name_cstring() const override
  { 
    static LEX_CSTRING name_distinct= { STRING_WITH_LEN("avg(distinct ")};
    static LEX_CSTRING name_normal=   { STRING_WITH_LEN("avg(") };
    return has_with_distinct() ? name_distinct : name_normal;
  }
  Item *copy_or_same(THD* thd) override;
  Field *create_tmp_field(MEM_ROOT *root, bool group, TABLE *table) override;
  void cleanup() override
  {
    count= 0;
    Item_sum_sum::cleanup();
  }
  Item *get_copy(THD *thd) override
  { return get_item_copy<Item_sum_avg>(thd, this); }

  bool supports_removal() const override
  {
    return true;
  }
};


/*
  variance(a) =

  =  sum (ai - avg(a))^2 / count(a) )
  =  sum (ai^2 - 2*ai*avg(a) + avg(a)^2) / count(a)
  =  (sum(ai^2) - sum(2*ai*avg(a)) + sum(avg(a)^2))/count(a) = 
  =  (sum(ai^2) - 2*avg(a)*sum(a) + count(a)*avg(a)^2)/count(a) = 
  =  (sum(ai^2) - 2*sum(a)*sum(a)/count(a) + count(a)*sum(a)^2/count(a)^2 )/count(a) = 
  =  (sum(ai^2) - 2*sum(a)^2/count(a) + sum(a)^2/count(a) )/count(a) = 
  =  (sum(ai^2) - sum(a)^2/count(a))/count(a)

But, this falls prey to catastrophic cancellation.  Instead, use the recurrence formulas

  M_{1} = x_{1}, ~ M_{k} = M_{k-1} + (x_{k} - M_{k-1}) / k newline 
  S_{1} = 0, ~ S_{k} = S_{k-1} + (x_{k} - M_{k-1}) times (x_{k} - M_{k}) newline
  for 2 <= k <= n newline
  ital variance = S_{n} / (n-1)

*/

class Stddev
{
  double m_m;
  double m_s;
  ulonglong m_count;
public:
  Stddev() :m_m(0), m_s(0), m_count(0) { }
  Stddev(double nr) :m_m(nr), m_s(0.0), m_count(1) { }
  Stddev(const uchar *);
  void to_binary(uchar *) const;
  void recurrence_next(double nr);
  double result(bool is_simple_variance);
  ulonglong count() const { return m_count; }
  static uint32 binary_size()
  {
    return (uint32) (sizeof(double) * 2 + sizeof(ulonglong));
  };
};



class Item_sum_variance :public Item_sum_double
{
  Stddev m_stddev;
  bool fix_length_and_dec(THD *thd) override;

public:
  uint sample;
  uint prec_increment;

  Item_sum_variance(THD *thd, Item *item_par, uint sample_arg):
    Item_sum_double(thd, item_par),
    sample(sample_arg)
    {}
  Item_sum_variance(THD *thd, Item_sum_variance *item);
  Sumfunctype sum_func () const override { return VARIANCE_FUNC; }
  void fix_length_and_dec_double();
  void fix_length_and_dec_decimal();
  void clear() override final;
  bool add() override final;
  double val_real() override;
  void reset_field() override final;
  void update_field() override final;
  Item *result_item(THD *thd, Field *field) override;
  void no_rows_in_result() override final {}
  LEX_CSTRING func_name_cstring() const override
  {
    static LEX_CSTRING name_sample= { STRING_WITH_LEN("var_samp(")};
    static LEX_CSTRING name_normal=   { STRING_WITH_LEN("variance(") };
    return sample ? name_sample : name_normal;
  }
  Item *copy_or_same(THD* thd) override;
  Field *create_tmp_field(MEM_ROOT *root, bool group, TABLE *table) override
    final;
  void cleanup() override final
  {
    m_stddev= Stddev();
    Item_sum_double::cleanup();
  }
  Item *get_copy(THD *thd) override
  { return get_item_copy<Item_sum_variance>(thd, this); }
};

/*
   standard_deviation(a) = sqrt(variance(a))
*/

class Item_sum_std final :public Item_sum_variance
{
  public:
  Item_sum_std(THD *thd, Item *item_par, uint sample_arg):
    Item_sum_variance(thd, item_par, sample_arg) {}
  Item_sum_std(THD *thd, Item_sum_std *item)
    :Item_sum_variance(thd, item)
    {}
  enum Sumfunctype sum_func () const override final { return STD_FUNC; }
  double val_real() override final;
  Item *result_item(THD *thd, Field *field) override final;
  LEX_CSTRING func_name_cstring() const override
  {
    static LEX_CSTRING std_name= {STRING_WITH_LEN("std(") };
    static LEX_CSTRING stddev_samp_name= {STRING_WITH_LEN("stddev_samp(") };
    return sample ? stddev_samp_name : std_name;
  }
  Item *copy_or_same(THD* thd) override final;
  Item *get_copy(THD *thd) override final
  { return get_item_copy<Item_sum_std>(thd, this); }
};


class Item_sum_hybrid : public Item_sum,
                       public Type_handler_hybrid_field_type
{
public:
  Item_sum_hybrid(THD *thd, Item *item_par):
    Item_sum(thd, item_par),
    Type_handler_hybrid_field_type(&type_handler_slonglong)
  { collation.set(&my_charset_bin); }
  Item_sum_hybrid(THD *thd, Item *a, Item *b):
    Item_sum(thd, a, b),
    Type_handler_hybrid_field_type(&type_handler_slonglong)
  { collation.set(&my_charset_bin); }
  Item_sum_hybrid(THD *thd, Item_sum_hybrid *item)
    :Item_sum(thd, item),
    Type_handler_hybrid_field_type(item)
  { }
  const Type_handler *type_handler() const override
  { return Type_handler_hybrid_field_type::type_handler(); }
  bool fix_length_and_dec_generic();
  bool fix_length_and_dec_numeric(const Type_handler *h);
  bool fix_length_and_dec_string();
};


// This class is a string or number function depending on num_func
class Arg_comparator;
class Item_cache;
class Item_sum_min_max :public Item_sum_hybrid
{
protected:
  bool direct_added;
  Item *direct_item;
  Item_cache *value, *arg_cache;
  Arg_comparator *cmp;
  int cmp_sign;
  bool was_values;  // Set if we have found at least one row (for max/min only)
  bool was_null_value;

public:
  Item_sum_min_max(THD *thd, Item *item_par,int sign):
    Item_sum_hybrid(thd, item_par),
    direct_added(FALSE), value(0), arg_cache(0), cmp(0),
    cmp_sign(sign), was_values(TRUE)
  { collation.set(&my_charset_bin); }
  Item_sum_min_max(THD *thd, Item_sum_min_max *item)
    :Item_sum_hybrid(thd, item),
    direct_added(FALSE), value(item->value), arg_cache(0),
    cmp_sign(item->cmp_sign), was_values(item->was_values)
  { }
  bool fix_fields(THD *, Item **) override;
  bool fix_length_and_dec(THD *thd) override;
  void setup_hybrid(THD *thd, Item *item, Item *value_arg);
  void clear() override;
  void direct_add(Item *item);
  double val_real() override;
  longlong val_int() override;
  my_decimal *val_decimal(my_decimal *) override;
  bool get_date(THD *thd, MYSQL_TIME *ltime, date_mode_t fuzzydate) override;
  void reset_field() override;
  String *val_str(String *) override;
  bool val_native(THD *thd, Native *) override;
  const Type_handler *real_type_handler() const override
  {
    return get_arg(0)->real_type_handler();
  }
  const TYPELIB *get_typelib() const  override { return args[0]->get_typelib(); }
  void update_field() override;
  void min_max_update_str_field();
  void min_max_update_real_field();
  void min_max_update_int_field();
  void min_max_update_decimal_field();
  void min_max_update_native_field();
  void cleanup() override;
  bool any_value() { return was_values; }
  void no_rows_in_result() override;
  void restore_to_before_no_rows_in_result() override;
  Field *create_tmp_field(MEM_ROOT *root, bool group, TABLE *table) override;
  void setup_caches(THD *thd) override
  { setup_hybrid(thd, arguments()[0], NULL); }
};


class Item_sum_min final :public Item_sum_min_max
{
public:
  Item_sum_min(THD *thd, Item *item_par): Item_sum_min_max(thd, item_par, 1) {}
  Item_sum_min(THD *thd, Item_sum_min *item) :Item_sum_min_max(thd, item) {}
  enum Sumfunctype sum_func () const override {return MIN_FUNC;}

  bool add() override;
  LEX_CSTRING func_name_cstring() const override
  {
    static LEX_CSTRING sum_name= {STRING_WITH_LEN("min(") };
    return sum_name;
  }
  Item *copy_or_same(THD* thd) override;
  Item *get_copy(THD *thd) override
  { return get_item_copy<Item_sum_min>(thd, this); }
};


class Item_sum_max final :public Item_sum_min_max
{
public:
  Item_sum_max(THD *thd, Item *item_par): Item_sum_min_max(thd, item_par, -1) {}
  Item_sum_max(THD *thd, Item_sum_max *item) :Item_sum_min_max(thd, item) {}
  enum Sumfunctype sum_func () const  override {return MAX_FUNC;}

  bool add() override;
  LEX_CSTRING func_name_cstring() const override
  {
    static LEX_CSTRING sum_name= {STRING_WITH_LEN("max(") };
    return sum_name;
  }
  Item *copy_or_same(THD* thd) override;
  Item *get_copy(THD *thd) override
  { return get_item_copy<Item_sum_max>(thd, this); }
};


class Item_sum_bit :public Item_sum_int
{
public:
  Item_sum_bit(THD *thd, Item *item_par, ulonglong reset_arg):
    Item_sum_int(thd, item_par), reset_bits(reset_arg), bits(reset_arg),
    as_window_function(FALSE), num_values_added(0) {}
  Item_sum_bit(THD *thd, Item_sum_bit *item):
    Item_sum_int(thd, item), reset_bits(item->reset_bits), bits(item->bits),
    as_window_function(item->as_window_function),
    num_values_added(item->num_values_added)
  {
    if (as_window_function)
      memcpy(bit_counters, item->bit_counters, sizeof(bit_counters));
  }
  enum Sumfunctype sum_func () const override { return SUM_BIT_FUNC;}
  void clear() override;
  longlong val_int() override;
  void reset_field() override;
  void update_field() override;
  const Type_handler *type_handler() const override
  { return &type_handler_ulonglong; }
  bool fix_length_and_dec(THD *thd) override
  {
    if (args[0]->check_type_can_return_int(func_name_cstring()))
      return true;
    decimals= 0; max_length=21; unsigned_flag= 1;
    base_flags&= ~item_base_t::MAYBE_NULL;
    null_value= 0;
    return FALSE;
  }
  void cleanup() override
  {
    bits= reset_bits;
    if (as_window_function)
      clear_as_window();
    Item_sum_int::cleanup();
  }
  void setup_window_func(THD *thd __attribute__((unused)),
                         Window_spec *window_spec __attribute__((unused)))
    override
  {
    as_window_function= TRUE;
    clear_as_window();
  }
  void remove() override
  {
    if (as_window_function)
    {
      remove_as_window(args[0]->val_int());
      return;
    }
    // Unless we're counting bits, we can not remove anything.
    DBUG_ASSERT(0);
  }

  bool supports_removal() const override
  {
    return true;
  }

protected:
  enum bit_counters { NUM_BIT_COUNTERS= 64 };
  ulonglong reset_bits,bits;
  /*
    Marks whether the function is to be computed as a window function.
  */
  bool as_window_function;
  // When used as an aggregate window function, we need to store
  // this additional information.
  ulonglong num_values_added;
  ulonglong bit_counters[NUM_BIT_COUNTERS];
  bool add_as_window(ulonglong value);
  bool remove_as_window(ulonglong value);
  bool clear_as_window();
  virtual void set_bits_from_counters()= 0;
};


class Item_sum_or final :public Item_sum_bit
{
public:
  Item_sum_or(THD *thd, Item *item_par): Item_sum_bit(thd, item_par, 0) {}
  Item_sum_or(THD *thd, Item_sum_or *item) :Item_sum_bit(thd, item) {}
  bool add() override;
  LEX_CSTRING func_name_cstring() const override
  {
    static LEX_CSTRING sum_name= {STRING_WITH_LEN("bit_or(") };
    return sum_name;
  }
  Item *copy_or_same(THD* thd) override;
  Item *get_copy(THD *thd) override
  { return get_item_copy<Item_sum_or>(thd, this); }

private:
  void set_bits_from_counters() override;
};


class Item_sum_and final :public Item_sum_bit
{
public:
  Item_sum_and(THD *thd, Item *item_par):
    Item_sum_bit(thd, item_par, ULONGLONG_MAX) {}
  Item_sum_and(THD *thd, Item_sum_and *item) :Item_sum_bit(thd, item) {}
  bool add() override;
  LEX_CSTRING func_name_cstring() const override
  {
    static LEX_CSTRING sum_min_name= {STRING_WITH_LEN("bit_and(") };
    return sum_min_name;
  }
  Item *copy_or_same(THD* thd) override;
  Item *get_copy(THD *thd) override
  { return get_item_copy<Item_sum_and>(thd, this); }

private:
  void set_bits_from_counters() override;
};

class Item_sum_xor final :public Item_sum_bit
{
public:
  Item_sum_xor(THD *thd, Item *item_par): Item_sum_bit(thd, item_par, 0) {}
  Item_sum_xor(THD *thd, Item_sum_xor *item) :Item_sum_bit(thd, item) {}
  bool add() override;
  LEX_CSTRING func_name_cstring() const override
  {
    static LEX_CSTRING sum_min_name= {STRING_WITH_LEN("bit_xor(") };
    return sum_min_name;
  }
  Item *copy_or_same(THD* thd) override;
  Item *get_copy(THD *thd) override
  { return get_item_copy<Item_sum_xor>(thd, this); }

private:
  void set_bits_from_counters() override;
};

class sp_head;
class sp_name;
class Query_arena;
struct st_sp_security_context;

/*
  Item_sum_sp handles STORED AGGREGATE FUNCTIONS

  Each Item_sum_sp represents a custom aggregate function. Inside the
  function's body, we require at least one occurrence of FETCH GROUP NEXT ROW
  instruction. This cursor is what makes custom stored aggregates possible.

  During computation the function's add method is called. This in turn performs
  an execution of the function. The function will execute from the current
  function context (and instruction), if one exists, or from the start if not.
  See Item_sp for more details.

  Upon encounter of FETCH GROUP NEXT ROW instruction, the function will pause
  execution. We assume that the user has performed the necessary additions for
  a row, between two encounters of FETCH GROUP NEXT ROW.

  Example:
  create aggregate function f1(x INT) returns int
  begin
    declare continue handler for not found return s;
    declare s int default 0
    loop
      fetch group next row;
      set s = s + x;
    end loop;
  end

  The function will always stop after an encounter of FETCH GROUP NEXT ROW,
  except (!) on first encounter, as the value for the first row in the
  group is already set in the argument x. This behaviour is done so when
  a user writes a function, he should "logically" include FETCH GROUP NEXT ROW
  before any "add" instructions in the stored function. This means however that
  internally, the first occurrence doesn't stop the function. See the
  implementation of FETCH GROUP NEXT ROW for details as to how it happens.

  Either way, one should assume that after calling "Item_sum_sp::add()" that
  the values for that particular row have been added to the aggregation.

  To produce values for val_xxx methods we need an extra syntactic construct.
  We require a continue handler when "no more rows are available". val_xxx
  methods force a function return by executing the function again, while
  setting a server flag that no more rows have been found. This implies
  that val_xxx methods should only be called once per group however.

  Example:
  DECLARE CONTINUE HANDLER FOR NOT FOUND RETURN ret_val;
*/
class Item_sum_sp :public Item_sum,
                   public Item_sp
{
 private:
  bool execute();

public:
  Item_sum_sp(THD *thd, Name_resolution_context *context_arg, sp_name *name,
              sp_head *sp);

  Item_sum_sp(THD *thd, Name_resolution_context *context_arg, sp_name *name,
              sp_head *sp, List<Item> &list);
  Item_sum_sp(THD *thd, Item_sum_sp *item);

  enum Sumfunctype sum_func () const override
  {
    return SP_AGGREGATE_FUNC;
  }
  Field *create_field_for_create_select(MEM_ROOT *root, TABLE *table) override
  {
    return create_table_field_from_handler(root, table);
  }
  bool fix_length_and_dec(THD *thd) override;
  bool fix_fields(THD *thd, Item **ref) override;
  LEX_CSTRING func_name_cstring() const override;
  const Type_handler *type_handler() const override;
  bool add() override;

  /* val_xx functions */
  longlong val_int() override
  {
    if(execute())
      return 0;
    return sp_result_field->val_int();
  }

  double val_real() override
  {
    if(execute())
      return 0.0;
    return sp_result_field->val_real();
  }

  my_decimal *val_decimal(my_decimal *dec_buf) override
  {
    if(execute())
      return NULL;
    return sp_result_field->val_decimal(dec_buf);
  }

  bool val_native(THD *thd, Native *to) override
  {
    return (null_value= execute()) || sp_result_field->val_native(to);
  }

  String *val_str(String *str) override
  {
    String buf;
    char buff[20];
    buf.set(buff, 20, str->charset());
    buf.length(0);
    if (execute())
      return NULL;
    /*
      result_field will set buf pointing to internal buffer
      of the resul_field. Due to this it will change any time
      when SP is executed. In order to prevent occasional
      corruption of returned value, we make here a copy.
    */
    sp_result_field->val_str(&buf);
    str->copy(buf);
    return str;
  }
  void reset_field() override{DBUG_ASSERT(0);}
  void update_field() override{DBUG_ASSERT(0);}
  void clear() override;
  void cleanup() override;
  bool get_date(THD *thd, MYSQL_TIME *ltime, date_mode_t fuzzydate) override
  {
    return execute() || sp_result_field->get_date(ltime, fuzzydate);
  }
  inline Field *get_sp_result_field()
  {
    return sp_result_field;
  }
  Item *get_copy(THD *thd) override
  { return get_item_copy<Item_sum_sp>(thd, this); }
  Item *copy_or_same(THD *thd) override;
};

/* Items to get the value of a stored sum function */

class Item_sum_field :public Item
{
protected:
  Field *field;
public:
  Item_sum_field(THD *thd, Item_sum *item)
    :Item(thd), field(item->result_field)
  {
    name= item->name;
    set_maybe_null();
    decimals= item->decimals;
    max_length= item->max_length;
    unsigned_flag= item->unsigned_flag;
  }
  table_map used_tables() const override { return (table_map) 1L; }
  Field *create_tmp_field_ex(MEM_ROOT *root, TABLE *table, Tmp_field_src *src,
                             const Tmp_field_param *param) override
  {
    return create_tmp_field_ex_simple(root, table, src, param);
  }
  void save_in_result_field(bool no_conversions) override { DBUG_ASSERT(0); }
  bool check_vcol_func_processor(void *arg) override
  {
    return mark_unsupported_function(name.str, arg, VCOL_IMPOSSIBLE);
  }
  bool get_date(THD *thd, MYSQL_TIME *ltime, date_mode_t fuzzydate) override
  {
    return type_handler()->Item_get_date_with_warn(thd, this, ltime, fuzzydate);
  }
};


class Item_avg_field :public Item_sum_field
{
protected:
  uint prec_increment;
public:
  Item_avg_field(THD *thd, Item_sum_avg *item)
   :Item_sum_field(thd, item), prec_increment(item->prec_increment)
  { }
  enum Type type() const override { return FIELD_AVG_ITEM; }
  bool is_null() override { update_null_value(); return null_value; }
};


class Item_avg_field_double :public Item_avg_field
{
public:
  Item_avg_field_double(THD *thd, Item_sum_avg *item)
   :Item_avg_field(thd, item)
  { }
  const Type_handler *type_handler() const override
  { return &type_handler_double; }
  longlong val_int() override { return val_int_from_real(); }
  my_decimal *val_decimal(my_decimal *dec) override
  { return val_decimal_from_real(dec); }
  String *val_str(String *str) override
  { return val_string_from_real(str); }
  double val_real() override;
  Item *get_copy(THD *thd) override
  { return get_item_copy<Item_avg_field_double>(thd, this); }
};


class Item_avg_field_decimal :public Item_avg_field
{
  uint f_precision, f_scale, dec_bin_size;
public:
  Item_avg_field_decimal(THD *thd, Item_sum_avg *item)
   :Item_avg_field(thd, item),
    f_precision(item->f_precision),
    f_scale(item->f_scale),
    dec_bin_size(item->dec_bin_size)
  { }
  const Type_handler *type_handler() const override
  { return &type_handler_newdecimal; }
  double val_real() override
  {
    return VDec(this).to_double();
  }
  longlong val_int() override
  {
    return VDec(this).to_longlong(unsigned_flag);
  }
  String *val_str(String *str) override
  {
    return VDec(this).to_string_round(str, decimals);
  }
  my_decimal *val_decimal(my_decimal *) override;
  Item *get_copy(THD *thd) override
  { return get_item_copy<Item_avg_field_decimal>(thd, this); }
};


class Item_variance_field :public Item_sum_field
{
  uint sample;
public:
  Item_variance_field(THD *thd, Item_sum_variance *item)
   :Item_sum_field(thd, item), sample(item->sample)
  { }
  enum Type type() const override {return FIELD_VARIANCE_ITEM; }
  double val_real() override;
  longlong val_int() override { return val_int_from_real(); }
  String *val_str(String *str) override
  { return val_string_from_real(str); }
  my_decimal *val_decimal(my_decimal *dec_buf) override
  { return val_decimal_from_real(dec_buf); }
  bool is_null() override { update_null_value(); return null_value; }
  const Type_handler *type_handler() const override
  { return &type_handler_double; }
  Item *get_copy(THD *thd) override
  { return get_item_copy<Item_variance_field>(thd, this); }
};


class Item_std_field :public Item_variance_field
{
public:
  Item_std_field(THD *thd, Item_sum_std *item)
   :Item_variance_field(thd, item)
  { }
  enum Type type() const override { return FIELD_STD_ITEM; }
  double val_real() override;
  Item *get_copy(THD *thd) override
  { return get_item_copy<Item_std_field>(thd, this); }
};


/*
  User defined aggregates
*/

#ifdef HAVE_DLOPEN

class Item_udf_sum : public Item_sum
{
protected:
  udf_handler udf;

public:
  Item_udf_sum(THD *thd, udf_func *udf_arg):
    Item_sum(thd), udf(udf_arg)
  { quick_group=0; }
  Item_udf_sum(THD *thd, udf_func *udf_arg, List<Item> &list):
    Item_sum(thd, list), udf(udf_arg)
  { quick_group=0;}
  Item_udf_sum(THD *thd, Item_udf_sum *item)
    :Item_sum(thd, item), udf(item->udf)
  { udf.not_original= TRUE; }
  LEX_CSTRING func_name_cstring() const override
  {
    const char *tmp= udf.name();
    return {tmp, strlen(tmp) };
  }
  bool fix_fields(THD *thd, Item **ref) override
  {
    DBUG_ASSERT(fixed() == 0);

    if (init_sum_func_check(thd))
      return TRUE;

    base_flags|= item_base_t::FIXED;
    /*
      We set const_item_cache to false in constructors.
      It can be later changed to "true", in a Item_sum::make_const() call.
      No make_const() calls should have happened so far.
    */
    DBUG_ASSERT(!const_item_cache);
    if (udf.fix_fields(thd, this, this->arg_count, this->args))
      return TRUE;
    /**
      The above call for udf.fix_fields() updates
      the Used_tables_and_const_cache part of "this" as if it was a regular
      non-aggregate UDF function and can change both const_item_cache and
      used_tables_cache members.
      - The used_tables_cache will be re-calculated in update_used_tables()
        which is called from check_sum_func() below. So we don't care about
        its current value.
      - The const_item_cache must stay "false" until a Item_sum::make_const()
        call happens, if ever. So we need to reset const_item_cache back to
        "false" here.
    */
    const_item_cache= false;
    memcpy (orig_args, args, sizeof (Item *) * arg_count);
    return check_sum_func(thd, ref);
  }
  enum Sumfunctype sum_func () const override { return UDF_SUM_FUNC; }
  virtual bool have_field_update(void) const { return 0; }

  void clear() override;
  bool add() override;
  bool supports_removal() const override;
  void remove() override;
  void reset_field() override {};
  void update_field() override {}
  void cleanup() override;
  void print(String *str, enum_query_type query_type) override;
  bool get_date(THD *thd, MYSQL_TIME *ltime, date_mode_t fuzzydate) override
  {
    return type_handler()->Item_get_date_with_warn(thd, this, ltime, fuzzydate);
  }
};


class Item_sum_udf_float :public Item_udf_sum
{
 public:
  Item_sum_udf_float(THD *thd, udf_func *udf_arg):
    Item_udf_sum(thd, udf_arg) {}
  Item_sum_udf_float(THD *thd, udf_func *udf_arg, List<Item> &list):
    Item_udf_sum(thd, udf_arg, list) {}
  Item_sum_udf_float(THD *thd, Item_sum_udf_float *item)
    :Item_udf_sum(thd, item) {}
  longlong val_int() override { return val_int_from_real(); }
  double val_real() override;
  String *val_str(String*str) override;
  my_decimal *val_decimal(my_decimal *) override;
  const Type_handler *type_handler() const override
  { return &type_handler_double; }
  bool fix_length_and_dec(THD *thd) override
  { fix_num_length_and_dec(); return FALSE; }
  Item *copy_or_same(THD* thd) override;
  Item *get_copy(THD *thd) override
  { return get_item_copy<Item_sum_udf_float>(thd, this); }
};


class Item_sum_udf_int :public Item_udf_sum
{
public:
  Item_sum_udf_int(THD *thd, udf_func *udf_arg):
    Item_udf_sum(thd, udf_arg) {}
  Item_sum_udf_int(THD *thd, udf_func *udf_arg, List<Item> &list):
    Item_udf_sum(thd, udf_arg, list) {}
  Item_sum_udf_int(THD *thd, Item_sum_udf_int *item)
    :Item_udf_sum(thd, item) {}
  longlong val_int() override;
  double val_real() override
  { DBUG_ASSERT(fixed()); return (double) Item_sum_udf_int::val_int(); }
  String *val_str(String*str) override;
  my_decimal *val_decimal(my_decimal *) override;
  const Type_handler *type_handler() const override
  {
    if (unsigned_flag)
      return &type_handler_ulonglong;
    return &type_handler_slonglong;
  }
  bool fix_length_and_dec(THD *thd) override { decimals=0; max_length=21; return FALSE; }
  Item *copy_or_same(THD* thd) override;
  Item *get_copy(THD *thd) override
  { return get_item_copy<Item_sum_udf_int>(thd, this); }
};


class Item_sum_udf_str :public Item_udf_sum
{
public:
  Item_sum_udf_str(THD *thd, udf_func *udf_arg):
    Item_udf_sum(thd, udf_arg) {}
  Item_sum_udf_str(THD *thd, udf_func *udf_arg, List<Item> &list):
    Item_udf_sum(thd, udf_arg, list) {}
  Item_sum_udf_str(THD *thd, Item_sum_udf_str *item)
    :Item_udf_sum(thd, item) {}
  String *val_str(String *) override;
  double val_real() override
  {
    int err_not_used;
    char *end_not_used;
    String *res;
    res=val_str(&str_value);
    return res ? res->charset()->strntod((char*) res->ptr(),res->length(),
			                 &end_not_used, &err_not_used) : 0.0;
  }
  longlong val_int() override
  {
    int err_not_used;
    char *end;
    String *res;
    CHARSET_INFO *cs;

    if (!(res= val_str(&str_value)))
      return 0;                                 /* Null value */
    cs= res->charset();
    end= (char*) res->ptr()+res->length();
    return cs->strtoll10(res->ptr(), &end, &err_not_used);
  }
  my_decimal *val_decimal(my_decimal *dec) override;
  const Type_handler *type_handler() const override
  { return string_type_handler(); }
  bool fix_length_and_dec(THD *thd) override;
  Item *copy_or_same(THD* thd) override;
  Item *get_copy(THD *thd) override
  { return get_item_copy<Item_sum_udf_str>(thd, this); }
};


class Item_sum_udf_decimal :public Item_udf_sum
{
public:
  Item_sum_udf_decimal(THD *thd, udf_func *udf_arg):
    Item_udf_sum(thd, udf_arg) {}
  Item_sum_udf_decimal(THD *thd, udf_func *udf_arg, List<Item> &list):
    Item_udf_sum(thd, udf_arg, list) {}
  Item_sum_udf_decimal(THD *thd, Item_sum_udf_decimal *item)
    :Item_udf_sum(thd, item) {}
  String *val_str(String *str) override
  {
    return VDec(this).to_string_round(str, decimals);
  }
  double val_real() override
  {
    return VDec(this).to_double();
  }
  longlong val_int() override
  {
    return VDec(this).to_longlong(unsigned_flag);
  }
  my_decimal *val_decimal(my_decimal *) override;
  const Type_handler *type_handler() const override
  { return &type_handler_newdecimal; }
  bool fix_length_and_dec(THD *thd) override
  { fix_num_length_and_dec(); return FALSE; }
  Item *copy_or_same(THD* thd) override;
  Item *get_copy(THD *thd) override
  { return get_item_copy<Item_sum_udf_decimal>(thd, this); }
};

#else /* Dummy functions to get yy_*.cc files compiled */

class Item_sum_udf_float :public Item_sum_double
{
 public:
  Item_sum_udf_float(THD *thd, udf_func *udf_arg):
    Item_sum_double(thd) {}
  Item_sum_udf_float(THD *thd, udf_func *udf_arg, List<Item> &list):
    Item_sum_double(thd) {}
  Item_sum_udf_float(THD *thd, Item_sum_udf_float *item)
    :Item_sum_double(thd, item) {}
  enum Sumfunctype sum_func () const { return UDF_SUM_FUNC; }
  double val_real() { DBUG_ASSERT(fixed()); return 0.0; }
  void clear() {}
  bool add() { return 0; }  
  void reset_field() { DBUG_ASSERT(0); };
  void update_field() {}
};


class Item_sum_udf_int :public Item_sum_double
{
public:
  Item_sum_udf_int(THD *thd, udf_func *udf_arg):
    Item_sum_double(thd) {}
  Item_sum_udf_int(THD *thd, udf_func *udf_arg, List<Item> &list):
    Item_sum_double(thd) {}
  Item_sum_udf_int(THD *thd, Item_sum_udf_int *item)
    :Item_sum_double(thd, item) {}
  enum Sumfunctype sum_func () const { return UDF_SUM_FUNC; }
  longlong val_int() { DBUG_ASSERT(fixed()); return 0; }
  double val_real() { DBUG_ASSERT(fixed()); return 0; }
  void clear() {}
  bool add() { return 0; }  
  void reset_field() { DBUG_ASSERT(0); };
  void update_field() {}
};


class Item_sum_udf_decimal :public Item_sum_double
{
 public:
  Item_sum_udf_decimal(THD *thd, udf_func *udf_arg):
    Item_sum_double(thd) {}
  Item_sum_udf_decimal(THD *thd, udf_func *udf_arg, List<Item> &list):
    Item_sum_double(thd) {}
  Item_sum_udf_decimal(THD *thd, Item_sum_udf_float *item)
    :Item_sum_double(thd, item) {}
  enum Sumfunctype sum_func () const { return UDF_SUM_FUNC; }
  double val_real() { DBUG_ASSERT(fixed()); return 0.0; }
  my_decimal *val_decimal(my_decimal *) { DBUG_ASSERT(fixed()); return 0; }
  void clear() {}
  bool add() { return 0; }
  void reset_field() { DBUG_ASSERT(0); };
  void update_field() {}
};


class Item_sum_udf_str :public Item_sum_double
{
public:
  Item_sum_udf_str(THD *thd, udf_func *udf_arg):
    Item_sum_double(thd) {}
  Item_sum_udf_str(THD *thd, udf_func *udf_arg, List<Item> &list):
    Item_sum_double(thd) {}
  Item_sum_udf_str(THD *thd, Item_sum_udf_str *item)
    :Item_sum_double(thd, item) {}
  String *val_str(String *)
    { DBUG_ASSERT(fixed()); null_value=1; return 0; }
  double val_real() { DBUG_ASSERT(fixed()); null_value=1; return 0.0; }
  longlong val_int() { DBUG_ASSERT(fixed()); null_value=1; return 0; }
  bool fix_length_and_dec(THD *thd) override
  { base_flags|= item_base_t::MAYBE_NULL; max_length=0; return FALSE; }
  enum Sumfunctype sum_func () const { return UDF_SUM_FUNC; }
  void clear() {}
  bool add() { return 0; }  
  void reset_field() { DBUG_ASSERT(0); };
  void update_field() {}
};

#endif /* HAVE_DLOPEN */

C_MODE_START
int group_concat_key_cmp_with_distinct(void* arg, const void* key1,
                                       const void* key2);
int group_concat_key_cmp_with_distinct_with_nulls(void* arg, const void* key1,
                                                  const void* key2);
int group_concat_key_cmp_with_order(void* arg, const void* key1,
                                    const void* key2);
int group_concat_key_cmp_with_order_with_nulls(void *arg, const void *key1,
                                               const void *key2);
int dump_leaf_key(void* key_arg,
                  element_count count __attribute__((unused)),
                  void* item_arg);
C_MODE_END

class Item_func_group_concat : public Item_sum
{
protected:
  TMP_TABLE_PARAM *tmp_table_param;
  String result;
  String *separator;
  TREE tree_base;
  TREE *tree;
  size_t tree_len;
  Item **ref_pointer_array;

  /**
     If DISTINCT is used with this GROUP_CONCAT, this member is used to filter
     out duplicates. 
     @see Item_func_group_concat::setup
     @see Item_func_group_concat::add
     @see Item_func_group_concat::clear
   */
  Unique *unique_filter;
  TABLE *table;
  ORDER **order;
  Name_resolution_context *context;
  /** The number of ORDER BY items. */
  uint arg_count_order;
  /** The number of selected items, aka the expr list. */
  uint arg_count_field;
  uint row_count;
  bool distinct;
  bool warning_for_row;
  bool always_null;
  bool force_copy_fields;
  /** True if entire result of GROUP_CONCAT has been written to output buffer. */
  bool result_finalized;
  /** Limits the rows in the result */
  Item *row_limit;
  /** Skips a particular number of rows in from the result*/
  Item *offset_limit;
  bool limit_clause;
  /* copy of the offset limit */
  ulonglong copy_offset_limit;
  /*copy of the row limit */
  ulonglong copy_row_limit;

  /*
    Following is 0 normal object and pointer to original one for copy
    (to correctly free resources)
  */
  Item_func_group_concat *original;

  /*
    Used by Item_func_group_concat and Item_func_json_arrayagg. The latter
    needs null values but the former doesn't.
  */
  bool add(bool exclude_nulls);

  friend int group_concat_key_cmp_with_distinct(void* arg, const void* key1,
                                                const void* key2);
  friend int group_concat_key_cmp_with_distinct_with_nulls(void* arg,
                                                           const void* key1,
                                                           const void* key2);
  friend int group_concat_key_cmp_with_order(void* arg, const void* key1,
					     const void* key2);
  friend int group_concat_key_cmp_with_order_with_nulls(void *arg,
                                       const void *key1, const void *key2);
  friend int dump_leaf_key(void* key_arg,
                           element_count count __attribute__((unused)),
			   void* item_arg);

  bool repack_tree(THD *thd);

  /*
    Says whether the function should skip NULL arguments
    or add them to the result.
    Redefined in JSON_ARRAYAGG.
  */
  virtual bool skip_nulls() const { return true; }
  virtual String *get_str_from_item(Item *i, String *tmp)
    { return i->val_str(tmp); }
  virtual String *get_str_from_field(Item *i, Field *f, String *tmp,
                                     const uchar *key, size_t offset)
    { return f->val_str(tmp, key + offset); }
  virtual void cut_max_length(String *result,
                              uint old_length, uint max_length) const;
public:
  // Methods used by ColumnStore
  bool get_distinct() const { return distinct; }
  uint get_count_field() const { return arg_count_field; }
  uint get_order_field() const { return arg_count_order; }
  const String* get_separator() const { return separator; }
  ORDER** get_order() const { return order; }

public:
  Item_func_group_concat(THD *thd, Name_resolution_context *context_arg,
                         bool is_distinct, List<Item> *is_select,
                         const SQL_I_List<ORDER> &is_order, String *is_separator,
                         bool limit_clause, Item *row_limit, Item *offset_limit);

  Item_func_group_concat(THD *thd, Item_func_group_concat *item);
  ~Item_func_group_concat();
  void cleanup() override;

  enum Sumfunctype sum_func () const override {return GROUP_CONCAT_FUNC;}
  LEX_CSTRING func_name_cstring() const override
  {
    static LEX_CSTRING sum_name= {STRING_WITH_LEN("group_concat(") };
    return sum_name;
  }
  const Type_handler *type_handler() const override
  {
    if (too_big_for_varchar())
      return &type_handler_blob;
    return &type_handler_varchar;
  }
  void clear() override;
  bool add() override
  {
    return add(skip_nulls());
  }
  void reset_field() override { DBUG_ASSERT(0); }        // not used
  void update_field() override { DBUG_ASSERT(0); }       // not used
  bool fix_fields(THD *,Item **) override;
  bool setup(THD *thd) override;
  void make_unique() override;
  double val_real() override
  {
    int error;
    const char *end;
    String *res;
    if (!(res= val_str(&str_value)))
      return 0.0;
    end= res->ptr() + res->length();
    return (my_strtod(res->ptr(), (char**) &end, &error));
  }
  longlong val_int() override
  {
    String *res;
    char *end_ptr;
    int error;
    if (!(res= val_str(&str_value)))
      return (longlong) 0;
    end_ptr= (char*) res->ptr()+ res->length();
    return my_strtoll10(res->ptr(), &end_ptr, &error);
  }
  my_decimal *val_decimal(my_decimal *decimal_value) override
  {
    return val_decimal_from_string(decimal_value);
  }
  bool get_date(THD *thd, MYSQL_TIME *ltime, date_mode_t fuzzydate) override
  {
    return get_date_from_string(thd, ltime, fuzzydate);
  }
  String *val_str(String *str) override;
  Item *copy_or_same(THD* thd) override;
  void no_rows_in_result() override {}
  void print(String *str, enum_query_type query_type) override;
  bool change_context_processor(void *cntx) override
    { context= (Name_resolution_context *)cntx; return FALSE; }
  Item *get_copy(THD *thd) override
  { return get_item_copy<Item_func_group_concat>(thd, this); }
  qsort_cmp2 get_comparator_function_for_distinct();
  qsort_cmp2 get_comparator_function_for_order_by();
  uchar* get_record_pointer();
  uint get_null_bytes();

};

#endif /* ITEM_SUM_INCLUDED */
