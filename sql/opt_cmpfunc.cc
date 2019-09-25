#include "mariadb.h"
#include "sql_class.h"
#include "sql_select.h"
#include "item_cmpfunc.h"
#include "item.h"
#include "item_func.h"

/*
  This file describes the optimization that infer new inequalities
  from conjunctive predicates of the WHERE-clause.
  Conjuctive predicates should be linear inequalities.
  Linear ineqaulity is an inequality that contains linear functions only.

  E.g.

  3*a + 1 > 0
  3*a + 1 > 2*b + 5

  Conjunctive predicates can also be functions that can be transformed into
  linear inequalities.

  E.g.
  Non-linear functions that can be simply transformed into linear inequalities:

  2*(a + b) > 1     -> 2*a + 2*b > 1
  a BETWEEN 1 AND 2 -> (a >= 1) AND (b <= 2)
  a = 2             -> (a >= 2) AND (a <= 2)

  To infer new inequalities from the existing ones several steps should be done:
  1. Collect linear inequalities from WHERE clause and transform functions
     in linear inequalities if possible.
  2. Partition the collected inequalities fields into the minimum disjunctive
     sets.

    E.g.
    1. 3*a - b   < 2    AND
       4*c + 3*d > 0    AND
       b + 7*a   >= -14

       Here there are 2 disjunctive fields sets: {a,b} and {c,d}.

    2. 3*a - b   < 2    AND
       4*c + 3*d > 0    AND
       b + 7*a   >= -14 AND
       c = b

       Here (c = b) can be used and c can be substituted:

       3*a - b   < 2    AND
       4*b + 3*d > 0    AND
       b + 7*a   >= -14 AND
       c = b

       Single disjunctive fields set: {a,b,d}
  3. Create systems of inequalities that use fields from the same set collected
     in 2.
     E.g.
       3*a - b   < 2    AND
       4*c + 3*d > 0    AND
       b + 7*a   >= -14

       Inequalities systems:
       1. For {a,b} fields set:
          (3*a - b < 2) AND (b + 7*a >= 14)
       2. For {c,d} fields set:
          (4*c + 3*d > 0)
  4. Solve inequalities systems and get new inequalities of the form:
     <field> + <const> > 0

  4.1. Normalize inequalities: inequality should be with 'more' sign and be more
       than 0.

       E.g. {a,b} fields system
       3*a - b < 2       =>    0 <   2 - 3*a + 1*b
       b <= 0            =>    0 <=  0 + 0*a - 1*b

  4.2. For each inequality creates its vector using its factors and system
       fields. All vector fields should be of decimal type to simplify
       calculations, so inequality factors should be transformed into decimals.

       E.g. for {a,b} system:
                                    const   a     b
       0 <   2 - 3*a + 1*b   =>   (  2.0, -3.0,  1.0 )
       0 <=  0 + 0*a - 1*b   =>   (  0.0,  0.0, -1.0 )

  4.3. Tries to get new inequalities through:
  4.3.1. Backward wave: substitution of already found fields borders
         ( of the form a*field + b > 0) in other inequalities.
  4.3.2. Addition of inequalities.

  Stop when no new inequalities can be received.
  New inequalities can make WHERE clause always false.
*/


typedef ulong ineq_fields_map;
enum ineq_sign { LESS_SIGN, LESS_OR_EQUAL_SIGN, MORE_SIGN,
                 MORE_OR_EQUAL_SIGN, EQUAL_SIGN };

/*
  Count of digits after point in decimal number
*/
static const int count_of_decimal_digits= 38;

/**
  @class Ineq_vector
  @brief Representation of the inequality

  Each inequality is normalized. All factors are converted to decimals.
  E.g. 3*a - b < 2 will be transformed into -3*a + b + 2 > 0 and in
  the dynamic array containing sequences of vectors this inequality
  will be introduced by:
   a  b const
  -3  1  -2

  -a + 5*b >= -3 will be transformed into -a + 5*b + 3 >= 0 and in
  the dynamic array containing sequences of vectors this inequality
  will be introduced by:
   a  b const
  -1  5   3

  3*a + b = 5 will be transformed into two inequalities
  3*a + b - 5 >= 0 and -3*a - b + 5 >= 0
  that will be introduced in the dynamic array containing sequences
  of vectors by:
   a  b const   and   a  b const
  -1  5   3          -3 -1   5
*/

class Ineq_vector
{
public:
  uint first_elem_ref;  // first vector field index in the array of factors
  ineq_fields_map non_zero_map; // the bitmap of non-zero factors
  ineq_fields_map positive_map; // the bitmap of positive factors
  uint rank;  // the number of non-zero factors
  bool initial;  // set to true if it is original condition
  ineq_sign sign_of_ineq;

  void mark_as_non_zero_factor(uint i);

  Ineq_vector(uint fst_elem)
  {
    first_elem_ref= fst_elem;
    non_zero_map= 0;
    positive_map= 0;
    rank= 0;
    sign_of_ineq= MORE_SIGN;
    initial= false;
  }
  ineq_fields_map get_negative_map();
  bool is_constant(uint vector_length);
};


/**
  Set of inequalities that can be solved together
*/

class Linear_ineq_system :public Sql_alloc
{
public:
  List<Item> system_fields;
  List<Item> original_conds;
  uint marker;

  Linear_ineq_system(List<Item> *fi, Item *it) : marker(0)
  {
    List_iterator<Item> li(*fi);
    Item *item;
    while ((item=li++))
    {
      system_fields.push_back(item);
    }
    original_conds.push_back(it);
  }
};


class Ineq_vector_elem :public Sql_alloc
{
public:
  my_decimal initial_value;
  my_decimal upper_bound;
  my_decimal lower_bound;

  Ineq_vector_elem(my_decimal init, my_decimal up, my_decimal low) :
    initial_value(init), upper_bound(up), lower_bound(low)
  { }
};


ineq_fields_map get_fields_map(uint f_numb)
{
  return (ineq_fields_map) 1 << f_numb;
}


class Ineq_builder :public Sql_alloc
{
public:
  THD *thd;
  List<Item> working_list;
  List_iterator<Item> work_list_it;
  /* Current system fields. */
  List<Item> *system_fields;
  /* Recieved inequalities. */
  List<Item> *curr_conds;

  List<Linear_ineq_system> linear_systems; // systems list
  List_iterator<Linear_ineq_system> sys_it;
  Linear_ineq_system *system_for_field;

  uint vector_length;
  uint top_vector_idx;
  uint old_top_vector_idx;
  uint top_for_new_values;

  Item::cond_result cond_value;
  bool error;

  Item *last_field;
  /*
    Count of found restrictions for fields
  */
  uint resolved_fields_cnt;

  /*
    Dynamic array where elements of vectors are stored
  */
  Dynamic_array<Ineq_vector_elem> vector_elements;
  /*
    Dynamic array where vectors are stored
  */
  Dynamic_array<Ineq_vector> normalized_ineq_vectors;

  /*
    Constant where 0 in decimal interpretation is stored
  */
  my_decimal null_value;

  int prec_increment;
  /*
    Maximum vector length
  */
  static const uint max_fields_count= 64;
  /*
    Value that shows that there is still no restriction for a field
  */
  static const int no_field_value= -1;

  ineq_fields_map upper_bounds;
  ineq_fields_map lower_bounds;
  ineq_fields_map new_upper_bounds;
  ineq_fields_map new_lower_bounds;
  uint prev_top_vector_idx;

  /*
    Stores fields borders.
    It stores references on vectors where these restrictions are situated.
  */
  struct st_field_range
  {
    ineq_fields_map field_map;
    int upper_bound_ref; // Upper bound for the field
    int lower_bound_ref; // Lower bound for the field
  } field_range [max_fields_count];


  Ineq_builder(int pr, Item::cond_result *cnd_val): work_list_it(working_list),
                                                    sys_it(linear_systems)
  {
    prec_increment= pr;
    int2my_decimal(E_DEC_FATAL_ERROR, 0, 0, &null_value);
    cond_value= *cnd_val;
    error= false;
  }
  /*
    Set -1 in st_field_range struct
  */
  void init_field_structs()
  {
    for (uint i=0; i<vector_length; i++)
    {
      field_range[i].field_map= get_fields_map(i);
      field_range[i].upper_bound_ref= no_field_value;
      field_range[i].lower_bound_ref= no_field_value;
    }
  }

  void prepare_for_normalization(Linear_ineq_system *system)
  {
    working_list= system->system_fields;
    curr_conds= &system->original_conds;
    vector_length= system->system_fields.elements+1;
    top_vector_idx= 0;
    top_for_new_values= 0;
    resolved_fields_cnt= 0;
    vector_elements.clear();
    normalized_ineq_vectors.clear();
  }

  bool decimal_is_neg(my_decimal *numb);
  void my_decimal_abs(my_decimal *result, my_decimal *val);
  void vector_elem_abs(Ineq_vector_elem *result, Ineq_vector_elem *val);
  void add_vector_elements(Ineq_vector_elem *result, Ineq_vector_elem *term1,
                           Ineq_vector_elem *term2);
  void mult_vector_elements(Ineq_vector_elem *result, Ineq_vector_elem *factor,
                            Ineq_vector_elem *const_val);
  void div_vector_elements(Ineq_vector_elem *result, Ineq_vector_elem *devidend,
                           Ineq_vector_elem *const_val, int prec_increment);
  Ineq_vector_elem *get_vector_field_factor_pos(Ineq_vector *vector, int j);
  void put_constant_in_vector(Ineq_vector *vector, my_decimal *const_value);
  void put_field_factor_in_vector(Ineq_vector *vector, uint j, int number);
  void make_vector_elem_negative(Ineq_vector_elem *val);
  void make_vector_negative(Ineq_vector *vector);
  void copy_vector(Ineq_vector *where, Ineq_vector *what);
  bool find_equal_field_in_partitions(Item *field_item);
  bool check_linearity(Item *item);
  bool check_transformed_funcs_linearity(THD *thd,
                            						 Item *left_arg,
                            						 Item *right_arg_ge,
                            						 Item *right_arg_le);
  bool extract_linear_inequalities(THD *thd, Item *cond);
  Ineq_vector *get_vector_at(int n);
  void vector_elem_set_zero(Ineq_vector_elem *val);
  void refresh_vector(Ineq_vector *vector);
  void fold_vectors(Ineq_vector *base, Ineq_vector *summand);
  void multiply_vectors(Ineq_vector *base, Ineq_vector *factor,
                        Ineq_vector_elem *const_value);
  void devide_vectors(Ineq_vector *base, Ineq_vector *devider);
  bool work_with_interval(int interval_val, Ineq_vector *base,
			  Ineq_vector *subtr, bool date_sub_interval);
  void prepare_vector_internal_info(uint vector_idx);
  bool check_non_contradiction_of_restrictions(int lower_bound,
                                               int upper_bound);
  bool check_constant_vector(Ineq_vector *vector);
  bool vector_is_border(uint vector_idx);
  void new_vector_computation(Ineq_vector *base,
                  			      Ineq_vector *subst,
                  			      uint f_numb);
  bool possibility_of_solving_inequalities(Ineq_vector *vector1,
                                  			   Ineq_vector *vector2,
                                  			   uint f_numb);
  bool solve_system();
  bool vector_substitution(int v_numb, uint f_numb,
                  			   uint start, uint end);
  bool substitute_system_field_borders(ineq_fields_map upper_bounds,
                              		     ineq_fields_map lower_bounds,
                              		     uint start_idx, uint end_idx);
  bool backward_wave(ineq_fields_map *upper_bounds,
	             ineq_fields_map *lower_bounds);
  bool infer_inequalities_for_ineq_system(THD *thd,
                                          Linear_ineq_system *system);
};


/**
  Collect SELECT_LEX constraints and add them to WHERE clause.
*/

bool add_constraints(JOIN *join, Item **cond)
{
  THD *thd= join->thd;
  List_iterator<TABLE_LIST> ti(join->select_lex->leaf_tables);
  TABLE_LIST *tbl;

  List<Item> constraints_list;
  while ((tbl= ti++))
  {
    if (tbl->table->check_constraints)
    {
      for (Virtual_column_info **chk= tbl->table->check_constraints;
           *chk ; chk++)
      {
        if (constraints_list.push_back((*chk)->expr, thd->mem_root))
          return false;
      }
    }
  }

  if (constraints_list.elements == 0)
    return false;

  Item_cond_and *and_constr_list=
    new (thd->mem_root) Item_cond_and(thd, constraints_list);
  thd->change_item_tree(cond,
                        and_conds(thd, *cond, and_constr_list));
  (*cond)->fix_fields(thd, 0);

  return false;
}


//
//
// WORK WITH DECIMALS
//
//

void put_int_in_decimal(int number, my_decimal *vector_element)
{
  int2my_decimal(E_DEC_FATAL_ERROR, number, FALSE, vector_element);
}


void sub_decimals(my_decimal *result, my_decimal *dec1, my_decimal *dec2)
{
  my_decimal dec;
  if (result == dec1)
  {
    dec= *dec1;
    my_decimal_sub(E_DEC_FATAL_ERROR, result, &dec, dec2);
  }
  else if (result == dec2)
  {
    dec= *dec2;
    my_decimal_sub(E_DEC_FATAL_ERROR, result, dec1, &dec);
  }
}


void add_decimals(my_decimal *result, my_decimal *dec1, my_decimal *dec2)
{
  my_decimal dec;
  if (result == dec1)
  {
    dec= *dec1;
    my_decimal_add(E_DEC_FATAL_ERROR, result, &dec, dec2);
  }
  else if (result == dec2)
  {
    dec= *dec2;
    my_decimal_add(E_DEC_FATAL_ERROR, result, dec1, &dec);
  }
}


/* Checks if dec2 is zero and puts 0 into dec1. */
bool check_zero_val_and_set_zero(my_decimal *dec1, my_decimal *dec2)
{
  if (my_decimal_is_zero(dec2))
  {
    my_decimal_set_zero(dec1);
    return true;
  }
  return false;
}


void mult_decimals(my_decimal *result, my_decimal *factor,
		               my_decimal *const_value)
{
  if (check_zero_val_and_set_zero(result, factor))
    return;

  my_decimal dec;
  if (result == factor)
  {
    dec= *factor;
    my_decimal_mul(E_DEC_FATAL_ERROR, result, &dec, const_value);
  }
  else
    my_decimal_mul(E_DEC_FATAL_ERROR, result, factor, const_value);
}


void div_decimals(my_decimal *result, my_decimal *devidend,
		              my_decimal *const_value, int prec_increment)
{
  if (check_zero_val_and_set_zero(result, devidend))
    return;

  my_decimal dec;
  if (result == devidend)
  {
    dec= *devidend;
    my_decimal_div(E_DEC_FATAL_ERROR, result, &dec, const_value, prec_increment);
  }
  else
    my_decimal_div(E_DEC_FATAL_ERROR, result, devidend,
                   const_value, prec_increment);
}


void round_decimal(my_decimal *const_val, my_decimal *new_val)
{
  if (my_decimal_is_zero(const_val))
    *new_val= *const_val;
  else
    const_val->round_to(new_val, count_of_decimal_digits, TRUNCATE);
}


void final_ineq_rounding(my_decimal *const_val, my_decimal *new_val,
			                   ineq_sign sign_of_ineq)
{
  if (sign_of_ineq == MORE_SIGN || sign_of_ineq == MORE_OR_EQUAL_SIGN)
    my_decimal_neg(const_val);
  round_decimal(const_val, new_val);
}


/**
  Check if decimal is negative.
*/

bool Ineq_builder::decimal_is_neg(my_decimal *numb)
{
  if (my_decimal_cmp(numb, &null_value) == -1)
    return true;
  return false;
}


/**
  Get the absolute value of a decimal.
*/

void Ineq_builder::my_decimal_abs(my_decimal *result, my_decimal *val)
{
  if (decimal_is_neg(val))
    my_decimal_neg(val);
}


#define DIG_PER_DEC1 9
static const decimal_digit_t powers10[DIG_PER_DEC1+1]={
  1, 10, 100, 1000, 10000, 100000, 1000000, 10000000, 100000000, 1000000000};

/*
  Get delta for the element (last significant element)
*/

void get_delta(const decimal_t *a, decimal_t *delta)
{
  int i, nb;
  delta->intg= 0;
  delta->sign= 0;
  delta->frac= DECIMAL_MAX_POSSIBLE_PRECISION - a->intg - DIG_PER_DEC1;
  nb= (delta->frac - 1) / DIG_PER_DEC1;
  for (i=0; i<nb; i++)
    delta->buf[i]= 0;
  delta->buf[nb]= powers10[DIG_PER_DEC1 - (delta->frac % DIG_PER_DEC1) - 1];
}

/*
// Work with maps
*/

/**
  Check if map has non-zero coefficient before f_numb field.
*/

bool map_intersect_with_field(ineq_fields_map map1, uint f_numb)
{
  return map1 & (1<<f_numb);
}


ineq_fields_map get_inverse_map(ineq_fields_map new_map)
{
  return ~new_map;
}


/**
  Get count of non-zero elements in new_map.
*/

int get_rank(ineq_fields_map new_map, uint n)
{
  uint units= 0;
  if (new_map!= 0)
  {
    for (uint i= 1; i<n; i++)
    {
      if (map_intersect_with_field(new_map, i))
        units++;
    }
  }
  return units;
}


////// INEQUALITY VECTOR ELEMENT OPERATIONS

void Ineq_builder::vector_elem_abs(Ineq_vector_elem *result,
                                   Ineq_vector_elem *val)
{
  my_decimal_abs(&result->initial_value, &val->initial_value);
  my_decimal_abs(&result->upper_bound, &val->upper_bound);
  my_decimal_abs(&result->lower_bound, &val->lower_bound);
}


void Ineq_builder::add_vector_elements(Ineq_vector_elem *result,
                                       Ineq_vector_elem *term1,
                                       Ineq_vector_elem *term2)
{
  add_decimals(&result->initial_value, &term1->initial_value,
               &term2->initial_value);
  add_decimals(&result->upper_bound, &term1->upper_bound, &term2->upper_bound);
  add_decimals(&result->lower_bound, &term1->lower_bound, &term2->lower_bound);
}


void Ineq_builder::mult_vector_elements(Ineq_vector_elem *result,
                                        Ineq_vector_elem *factor,
                                        Ineq_vector_elem *const_val)
{
  mult_decimals(&result->initial_value, &factor->initial_value,
                &const_val->initial_value);
  mult_decimals(&result->upper_bound, &factor->upper_bound,
                &const_val->upper_bound);
  mult_decimals(&result->lower_bound, &factor->lower_bound,
                &const_val->lower_bound);  
}


void Ineq_builder::div_vector_elements(Ineq_vector_elem *result,
                                       Ineq_vector_elem *devidend,
                                       Ineq_vector_elem *const_val,
                                       int prec_increment)
{
  div_decimals(&result->initial_value, &devidend->initial_value,
               &const_val->initial_value, prec_increment);
  div_decimals(&result->upper_bound, &devidend->upper_bound,
               &const_val->upper_bound, prec_increment);
  div_decimals(&result->lower_bound, &devidend->lower_bound,
               &const_val->lower_bound, prec_increment);
}


/**
  Return vector negative factors map.
*/

ineq_fields_map Ineq_vector::get_negative_map()
{
  return get_inverse_map(positive_map | get_inverse_map(non_zero_map));
}


void Ineq_vector::mark_as_non_zero_factor(uint i)
{
  non_zero_map= 0;
  non_zero_map = ineq_fields_map (1<<i);
}


/**
  Check if vector is a constant value
*/

bool Ineq_vector::is_constant(uint vector_length)
{
  if (non_zero_map == 0 || non_zero_map == 1)
    return true;
  return false;
}


Ineq_vector_elem
*Ineq_builder::get_vector_field_factor_pos(Ineq_vector *vector, int j)
{
  return &vector_elements.at(vector->first_elem_ref+j);
}


void Ineq_builder::put_constant_in_vector(Ineq_vector *vector,
                                          my_decimal *const_value)
{
  my_decimal delta;
  Ineq_vector_elem *val= get_vector_field_factor_pos(vector, 0);
  val->initial_value= *const_value;
  get_delta(const_value, &delta);
  add_decimals(&val->upper_bound, const_value, &delta);
  sub_decimals(&val->lower_bound, const_value, &delta);
}


void Ineq_builder::put_field_factor_in_vector(Ineq_vector *vector,
					                                    uint j, int number)
{
  Ineq_vector_elem *val = get_vector_field_factor_pos(vector, j);
  put_int_in_decimal(number, &val->initial_value);
  put_int_in_decimal(number, &val->upper_bound);
  put_int_in_decimal(number, &val->lower_bound);
}


void Ineq_builder::make_vector_elem_negative(Ineq_vector_elem *val)
{
  my_decimal_neg(&val->initial_value);
  my_decimal_neg(&val->upper_bound);
  my_decimal_neg(&val->lower_bound);
  my_decimal temp= val->upper_bound;
  val->upper_bound= val->lower_bound;
  val->lower_bound= temp;
}


void Ineq_builder::make_vector_negative(Ineq_vector *vector)
{
  for (uint i=0; i<vector_length; i++)
  {
    Ineq_vector_elem *val = get_vector_field_factor_pos(vector, i);
    make_vector_elem_negative(val);
  }
}


/**
  Copy 'what' vector info to 'where' vector place.
*/

void Ineq_builder::copy_vector(Ineq_vector *where, Ineq_vector *what)
{
  for (uint i=0; i < vector_length; i++)
  {
    Ineq_vector_elem *what_val= get_vector_field_factor_pos(what,i);
    Ineq_vector_elem *where_val= get_vector_field_factor_pos(where, i);
    where_val->initial_value= what_val->initial_value;
    where_val->upper_bound= what_val->upper_bound;
    where_val->lower_bound= what_val->lower_bound;
  }

  where->non_zero_map= what->non_zero_map;
  where->positive_map= what->positive_map;
  where->rank= what->rank;
  where->sign_of_ineq= what->sign_of_ineq;
  where->initial= false;
}


/**
  Find field index in list. If not found return 0.
*/

uint find_equal_field_in_list(Item *field_item, List_iterator<Item> &it)
{
  it.rewind();
  Item *item;
  uint j= 1;
  Item_equal *eq_class= field_item->get_item_equal();

  while ((item=it++))
  {
    if (item->type() != field_item->type())
      continue;
    if ((eq_class &&
       ((Item_field *)(item->real_item()))->get_item_equal() == eq_class) ||
        (((Item_field *)(item->real_item()))->field ==
        ((Item_field *)(field_item->real_item()))->field))
      return j;
    j++;
  }
  return 0;
}


/**
  @brief
    Check if field occurs in the existed systems

  @param field_item  field to check

  @details
    For each system check if it contains field_item.

  @retval true   if the system was found
  @retval false  otherwise
*/

bool Ineq_builder::find_equal_field_in_partitions(Item *field_item)
{
  sys_it.rewind();
  Linear_ineq_system *system;

  while ((system=sys_it++))
  {
    List_iterator<Item> field_it(system->system_fields);
    if (find_equal_field_in_list(field_item, field_it) != 0)
    {
      system->marker++;
      if (field_item->type() == Item::FIELD_ITEM)
	      system_for_field= system;
      return true;
    }
  }
  return false;
}


bool Item_func_mul::linear_checker_processor(void *arg)
{
  if (args[0]->const_item() ||
      args[1]->const_item() )
    return false;
  return true;
}


bool Item_func_div::linear_checker_processor(void *arg)
{
  if (args[1]->const_item())
    return false;
  return true;
}


bool Item_direct_view_ref::linear_checker_processor(void *arg)
{
  if (real_item()->type() != Item::FIELD_ITEM)
    return true;

  Ineq_builder *builder= (Ineq_builder *)arg;

  /*
    As walk goes in Item_ref starting from the inner level
    (Item_field) it has already handled field f for which
    this item r is a shell. So we need to delete f as
    it isn't the item for which we should check our conditions.

    If the system for f is already found we should decrease marker
    that shows that the inequality can be solved in the system.
  */
  if (builder->system_for_field != 0)
    builder->system_for_field->marker--;
  else if (builder->last_field != 0)
    builder->working_list.pop();

  if (builder->find_equal_field_in_partitions(this))
    return false;

  if (find_equal_field_in_list(this, builder->work_list_it) != 0)
    return false;
  else if (builder->working_list.push_back(this))
  {
    builder->error= true;
    return true;
  }

  builder->last_field= this;
  return false;
}


bool Item_field::linear_checker_processor(void *arg)
{
  Ineq_builder *builder= (Ineq_builder *)arg;

  builder->system_for_field= 0;
  builder->last_field= 0;

  if (builder->find_equal_field_in_partitions(this))
    return false;

  if (find_equal_field_in_list(this, builder->work_list_it) != 0)
    return false;
  else if (builder->working_list.push_back(this))
  {
    builder->error= true;
    return true;
  }
  builder->last_field= this;
  return false;
}


/**
  @brief
    Check if inequality is linear

  @param item_func  the considered inequality

  @details
    Check if inequality is linear and add it to the system where
    it can be solved. If there is no such a system create
    a new system consists of the inequality only.

  @retval true   error occurs
  @retval false  otherwise
*/

bool Ineq_builder::check_linearity(Item *item)
{
  Item_func *item_func= (Item_func *)item;
  Linear_ineq_system *main_sys;

  working_list.empty();

  /*
    Check if inequality is linear
  */
  if (item_func->walk(&Item::linear_checker_processor,
                      0, (uchar *) this))
  {
    /*
      If error occurs
    */
    if (error)
      return true;

    sys_it.rewind();
    while ((main_sys= sys_it++))
      main_sys->marker= 0;
    working_list.empty();
    return false;
  }

  sys_it.rewind();

  /*
    Try to find the first system where inequality can be solved
  */
  while ((main_sys=sys_it++))
  {
    if (main_sys->marker > 0)
      break;
  }

  if (!main_sys)
  {
    if (!working_list.elements)
      return true;
    /*
      If there is no system found for the considered inequality
      create new system for this inequality.
    */
    if (linear_systems.push_back(new Linear_ineq_system(&working_list,
                                                        item_func)))
      return true;

    return false;
  }

  Linear_ineq_system *merge_sys;
  while ((merge_sys= sys_it++))
  {
    /*
      Find other systems except already found system main_sys where inequality
      can be solved.
      If there are any try to merge them to get one system consists of
      inequalities of merge_sys and main_sys linear systems.
    */
    if (merge_sys->marker > 0)
    {
      main_sys->original_conds.append(&merge_sys->original_conds);
      main_sys->system_fields.append(&merge_sys->system_fields);
      sys_it.remove();
    }
  }

  /*
    If there are still fields of inequality that don't appear
    in any other inequalities of the main_sys add these fields to system.
  */
  if (working_list.elements != 0)
    main_sys->system_fields.append(&working_list);
  /*
    Add inequality to system main_sys.
  */
  if (main_sys->original_conds.push_back(item_func))
    return true;

  main_sys->marker= 0;
  return false;
}


/**
  @brief
    Checks if the BETWEEN or EQUALITY function is linear

  @param thd           thread handle
  @param left_arg      left argument for the build inequalities
  @param right_arg_ge  right argument to build greater or equal equality
  @param right_arg_le  right argument to build less or equal equality

  @details
    Check if BETWEEN or EQUAL function is linear. It creates new
    inequalities in the way illustrated below and checks if the created
    inequalities are linear.

    E.g.
      a + 3*b BETWEEN 2 and 1 + c ->
      (a + 3*b >=2) AND (a + 3*b <= 1 + c)

      7*b + c = 4 ->
      (7*b + c >= 4) AND (7*b + c <= 4)

  @retval true   error occurs
  @retval false  otherwise
*/

bool Ineq_builder::check_transformed_funcs_linearity(THD *thd,
                                                     Item *left_arg,
                                                     Item *right_arg_ge,
                                                     Item *right_arg_le)
{
  Item_func_ge *ge_ineq=
    new (thd->mem_root) Item_func_ge(thd, left_arg, right_arg_ge);
  Item_func_le *le_ineq=
    new (thd->mem_root) Item_func_le(thd, left_arg, right_arg_le);

  if (ge_ineq->fix_fields(thd, 0) || le_ineq->fix_fields(thd, 0))
    return true;

  if (check_linearity(ge_ineq) || check_linearity(le_ineq))
    return true;
  return false;
}


/**
  @brief
    Collect WHERE clause linear inequalities conjuncts

  @param thd               thread handle
  @param cond              WHERE clause

  @details
    Check top AND-level WHERE inequalities, BETWEEN functions and
    equalities if they are linear and can be used in new
    inequalities deduction.

  @retval true  error occurs
  @retval false otherwise
*/

bool Ineq_builder::extract_linear_inequalities(THD *thd, Item *cond)
{
  if (cond->type() == Item::COND_ITEM ||
     ((Item_cond*) cond)->functype() != Item_func::COND_AND_FUNC)
    return true;

  List_iterator_fast<Item> it(*((Item_cond *) cond)->argument_list());
  Item *item;

  while ((item=it++))
  {
    if (item->type() != Item::FUNC_ITEM)
      continue;

    Item_func *func_item= (Item_func *)item;

    switch (func_item->functype())
    {
    case Item_func::BETWEEN:
      if (check_transformed_funcs_linearity(thd,
                                            func_item->arguments()[0],
                                            func_item->arguments()[1],
                                            func_item->arguments()[2]))
        return true;
      break;
    case Item_func::EQ_FUNC:
      if (check_transformed_funcs_linearity(thd,
                                            func_item->arguments()[0],
                                            func_item->arguments()[1],
                                            func_item->arguments()[1]))
        return true;
      break;
    case Item_func::GE_FUNC:
    case Item_func::GT_FUNC:
    case Item_func::LE_FUNC:
    case Item_func::LT_FUNC:
      if (check_linearity(item))
	      return true;
      break;
    default:
      break;
    }
  }

  if (linear_systems.elements == 0)
    return true;

  return false;
}


void Ineq_builder::vector_elem_set_zero(Ineq_vector_elem *val)
{
  my_decimal_set_zero(&val->initial_value);
  my_decimal_set_zero(&val->upper_bound);
  my_decimal_set_zero(&val->lower_bound);
}


/**
  Cleans up all information about this inequality.
*/

void Ineq_builder::refresh_vector(Ineq_vector *vector)
{
  uint beg= vector->first_elem_ref;
  for (uint i= beg; i < beg+vector_length; i++)
    vector_elem_set_zero(get_vector_field_factor_pos(vector, i));

  vector->non_zero_map= 0;
  vector->positive_map= 0;
  vector->rank= 0;
  vector->sign_of_ineq= MORE_SIGN;
  vector->initial= false;
}


Ineq_vector_elem init_new_vector_elem()
{
  my_decimal dec;
  my_decimal_set_zero(&dec);
  return Ineq_vector_elem(dec, dec, dec);
}


/**
  @brief
    Return vector that starts at n index

  @param n  start of the inequality in the array of inequalities

  @details
    The method returns inequality that starts at n position if
    there exists such an inequality. Otherwise, it creates a new one
    - a zero one, adds it to the array and returns it.

  @retval Ineq_vector  inequality
*/

Ineq_vector *Ineq_builder::get_vector_at(int n)
{
  int numb= normalized_ineq_vectors.elements();
  if (n < numb)
    return &normalized_ineq_vectors.at(n);

  Ineq_vector_elem *first_element= vector_elements.front();

  Ineq_vector new_vector(vector_elements.elements());

  normalized_ineq_vectors.push(new_vector);

  for (uint i=0; i<vector_length; i++)
  {
    Ineq_vector_elem val= init_new_vector_elem();
    vector_elements.push(val);
    vector_elements.back()->initial_value.fix_buffer_pointer();
    vector_elements.back()->upper_bound.fix_buffer_pointer();
    vector_elements.back()->lower_bound.fix_buffer_pointer();
  }

  if (first_element != vector_elements.front())
  {
    for (uint i=0; i < vector_elements.elements(); i++)
    {
      vector_elements.at(i).initial_value.fix_buffer_pointer();
      vector_elements.at(i).upper_bound.fix_buffer_pointer();
      vector_elements.at(i).lower_bound.fix_buffer_pointer();
    }
  }
  return &normalized_ineq_vectors.at(n);
}


bool Item_basic_constant::ineq_normalization_processor(void *arg)
{
  Ineq_builder *builder= (Ineq_builder *)arg;

  Ineq_vector *vector= builder->get_vector_at(builder->top_vector_idx);

  my_decimal const_value;
  my_decimal *ptr= &const_value;

  switch (field_type())
  {
    case MYSQL_TYPE_DECIMAL:
    case MYSQL_TYPE_TINY:
    case MYSQL_TYPE_SHORT:
    case MYSQL_TYPE_LONG:
    case MYSQL_TYPE_FLOAT:  
    case MYSQL_TYPE_DOUBLE:
    case MYSQL_TYPE_LONGLONG:
    case MYSQL_TYPE_INT24:
    case MYSQL_TYPE_NEWDECIMAL:
    case MYSQL_TYPE_DATE:
    case MYSQL_TYPE_DATETIME:
    case MYSQL_TYPE_TIME:
      ptr= val_decimal(ptr);
      break;
    default:
      return true;
  }

  builder->put_constant_in_vector(vector, ptr);
  vector->mark_as_non_zero_factor(0);
  builder->top_vector_idx++;
  return false;
}


bool Item_direct_view_ref::ineq_normalization_processor(void *arg)
{
  Ineq_builder *builder= (Ineq_builder *)arg;

  builder->work_list_it.rewind();
  uint j= find_equal_field_in_list(this, builder->work_list_it);
  if (j == 0)
    return true;/*mistake*/

  if (builder->last_field != 0)
  {
    builder->refresh_vector(builder->get_vector_at(builder->top_vector_idx-1));
    builder->top_vector_idx--;
  }

  Ineq_vector *vector= builder->get_vector_at(builder->top_vector_idx);
  builder->put_field_factor_in_vector(vector, j, 1);
  vector->mark_as_non_zero_factor(j);
  builder->top_vector_idx++;
  builder->last_field= 0;

  return false;
}


bool Item_field::ineq_normalization_processor(void *arg)
{
  Ineq_builder *builder= (Ineq_builder *)arg;
  builder->last_field= 0;

  builder->work_list_it.rewind();
  uint j= find_equal_field_in_list(this, builder->work_list_it);
  if (j == 0)
    return true;

  Ineq_vector *vector= builder->get_vector_at(builder->top_vector_idx);
  builder->put_field_factor_in_vector(vector, j, 1);
  vector->mark_as_non_zero_factor(j);
  builder->top_vector_idx++;
  builder->last_field= this;

  return false;
}


bool Item_func_neg::ineq_normalization_processor(void *arg)
{
  Ineq_builder *builder= (Ineq_builder *)arg;
  Ineq_vector *vector= builder->get_vector_at(builder->top_vector_idx-1);

  builder->make_vector_negative(vector);

  return false;
}


void Ineq_builder::fold_vectors(Ineq_vector *base, Ineq_vector *summand)
{
  base->non_zero_map |= summand->non_zero_map;

  for (uint i= 0; i<vector_length; i++)
  {
    if (!map_intersect_with_field(base->non_zero_map, i))
      continue;

    Ineq_vector_elem *base_elem= get_vector_field_factor_pos(base, i);

    add_vector_elements(base_elem,
                        get_vector_field_factor_pos(summand, i),
                        base_elem);

    if (my_decimal_is_zero(&base_elem->initial_value))
      base->non_zero_map &= ~(1<<i);
  }
}


bool Item_func_plus::ineq_normalization_processor(void *arg)
{
  Ineq_builder *builder= (Ineq_builder *)arg;
  Ineq_vector *base= builder->get_vector_at(builder->top_vector_idx-2);
  Ineq_vector *summand= builder->get_vector_at(builder->top_vector_idx-1);

  builder->fold_vectors(base, summand);

  builder->refresh_vector(summand);
  builder->top_vector_idx--;
  return false;
}


bool Item_func_minus::ineq_normalization_processor(void *arg)
{
  Ineq_builder *builder= (Ineq_builder *)arg;
  Ineq_vector *base= builder->get_vector_at(builder->top_vector_idx-2);
  Ineq_vector *subtr= builder->get_vector_at(builder->top_vector_idx-1);

  builder->make_vector_negative(subtr);

  builder->fold_vectors(base, subtr);

  builder->refresh_vector(subtr);
  builder->top_vector_idx--;
  return false;
}


bool Item_func_gt::ineq_normalization_processor(void *arg)
{
  Ineq_builder *builder= (Ineq_builder *)arg;
  Ineq_vector *base= builder->get_vector_at(builder->top_vector_idx-2);
  Ineq_vector *subtr= builder->get_vector_at(builder->top_vector_idx-1);

  builder->make_vector_negative(subtr);

  builder->fold_vectors(base, subtr);

  builder->refresh_vector(subtr);
  builder->top_vector_idx--;

  base->sign_of_ineq= MORE_SIGN;
  base->initial= true;
  return false;
}


bool Item_func_ge::ineq_normalization_processor(void *arg)
{
  Ineq_builder *builder= (Ineq_builder *)arg;
  Ineq_vector *base= builder->get_vector_at(builder->top_vector_idx-2);
  Ineq_vector *subtr= builder->get_vector_at(builder->top_vector_idx-1);

  builder->make_vector_negative(subtr);

  builder->fold_vectors(base, subtr);

  builder->refresh_vector(subtr);
  builder->top_vector_idx--;

  base->sign_of_ineq= MORE_OR_EQUAL_SIGN;
  base->initial= true;
  return false;
}


bool Item_func_lt::ineq_normalization_processor(void *arg)
{
  Ineq_builder *builder= (Ineq_builder *)arg;
  Ineq_vector *base= builder->get_vector_at(builder->top_vector_idx-2);
  Ineq_vector *subtr= builder->get_vector_at(builder->top_vector_idx-1);

  builder->make_vector_negative(base);

  builder->fold_vectors(base, subtr);

  builder->refresh_vector(subtr);
  builder->top_vector_idx--;

  base->sign_of_ineq= MORE_SIGN;
  base->initial= true;
  return false;
}


bool Item_func_le::ineq_normalization_processor(void *arg)
{
  Ineq_builder *builder= (Ineq_builder *)arg;
  Ineq_vector *base= builder->get_vector_at(builder->top_vector_idx-2);
  Ineq_vector *subtr= builder->get_vector_at(builder->top_vector_idx-1);

  builder->make_vector_negative(base);

  builder->fold_vectors(base, subtr);

  builder->refresh_vector(subtr);
  builder->top_vector_idx--;

  base->sign_of_ineq= MORE_OR_EQUAL_SIGN;
  base->initial= true;
  return false;
}


bool Item_func_add_time::ineq_normalization_processor(void *arg)
{
  Ineq_builder *builder= (Ineq_builder *)arg;
  Ineq_vector *base= builder->get_vector_at(builder->top_vector_idx-2);
  Ineq_vector *subtr= builder->get_vector_at(builder->top_vector_idx-1);

  if (sign == 1)
    builder->fold_vectors(base, subtr);
  else
  {
    builder->make_vector_negative(subtr);
    builder->fold_vectors(base, subtr);
  }

  builder->refresh_vector(subtr);
  builder->top_vector_idx--;
  return false;
}


void Ineq_builder::multiply_vectors(Ineq_vector *base, Ineq_vector *factor,
                                    Ineq_vector_elem *const_value)
{
  if (base->is_constant(vector_length))
    const_value= get_vector_field_factor_pos(base, 0);

  for (uint i=0; i<vector_length; i++)
  {
    Ineq_vector_elem *base_elem= get_vector_field_factor_pos(base, i);
    mult_vector_elements(base_elem,
                         get_vector_field_factor_pos(factor, i),
                         const_value);

    if (my_decimal_is_zero(&base_elem->initial_value))
      base->non_zero_map &= ~(1<<i);
  }
}


bool Item_func_mul::ineq_normalization_processor(void *arg)
{
  Ineq_builder *builder= (Ineq_builder *)arg;
  Ineq_vector *base= builder->get_vector_at(builder->top_vector_idx-2);
  Ineq_vector *factor= builder->get_vector_at(builder->top_vector_idx-1);

  if (base->is_constant(builder->vector_length))
  {
    builder->multiply_vectors(base, factor,
                              builder->get_vector_field_factor_pos(factor, 0));
    base->non_zero_map= factor->non_zero_map;
  }
  else
    builder->multiply_vectors(base, base,
                              builder->get_vector_field_factor_pos(base, 0));

  builder->refresh_vector(factor);
  builder->top_vector_idx--;
  return false;
}


void Ineq_builder::devide_vectors(Ineq_vector *base, Ineq_vector *devider)
{
  Ineq_vector_elem *const_value= get_vector_field_factor_pos(devider, 0);

  for (uint i=0; i<vector_length; i++)
  {
    Ineq_vector_elem *val= get_vector_field_factor_pos(base, i);
    div_vector_elements(val, val, const_value, prec_increment);
  }
}


bool Item_func_div::ineq_normalization_processor(void *arg)
{
  Ineq_builder *builder= (Ineq_builder *)arg;
  Ineq_vector *base= builder->get_vector_at(builder->top_vector_idx-2);
  Ineq_vector *devider= builder->get_vector_at(builder->top_vector_idx-1);

  builder->devide_vectors(base, devider);

  builder->refresh_vector(devider);
  builder->top_vector_idx--;
  return false;
}


bool Ineq_builder::work_with_interval(int interval_val,
                                      Ineq_vector *base,
                                      Ineq_vector *subtr,
                                      bool date_sub_interval)
{
  my_decimal ptr;
  int2my_decimal(E_DEC_FATAL_ERROR, interval_val, FALSE, &ptr);

  refresh_vector(subtr);
  put_constant_in_vector(subtr, &ptr);
  subtr->mark_as_non_zero_factor(subtr->first_elem_ref);

  if (!date_sub_interval)
    fold_vectors(base, subtr);
  else
  {
    make_vector_negative(subtr);
    fold_vectors(base, subtr);
  }

  refresh_vector(subtr);
  top_vector_idx--;

  return false;
}


bool Item_date_add_interval::ineq_normalization_processor(void *arg)
{
  THD *thd= current_thd;

  Ineq_builder *builder= (Ineq_builder *)arg;
  Ineq_vector *base= builder->get_vector_at(builder->top_vector_idx-2);
  Ineq_vector *subtr= builder->get_vector_at(builder->top_vector_idx-1);

  Item_date_add_interval *copy_add_interval=
      (Item_date_add_interval *)this->build_clone(thd);
  Item *args0= copy_add_interval->args[0];
  Item *real_it= args0;

  enum_field_types f_type= real_it->field_type();
  const char *str_d= "1000-01-01";
  int length;
  Item *new_date= 0;
  //get type
  switch (f_type)
  {
    case MYSQL_TYPE_DATE:
    case MYSQL_TYPE_NEWDATE:
      str_d= "1000-01-01";
      if (date_sub_interval)
	      str_d= "9999-12-31";
      length= 10;
      new_date= type_handler_date.create_literal_item(thd,
                  str_d, length, system_charset_info, true);
      break;
    case MYSQL_TYPE_TIME:
      str_d= "00:00:00";
      if (date_sub_interval)
	      str_d= "23:59:59";
      length= 8;
      new_date= type_handler_time.create_literal_item(thd,
                  str_d, length, system_charset_info, true);
      break;
    case MYSQL_TYPE_DATETIME:
      str_d= "1000-01-01 00:00:00";
      if (date_sub_interval)
	      str_d= "9999-12-31 23:59:59";
      length=19;
      new_date= type_handler_datetime.create_literal_item(thd,
                  str_d, length, system_charset_info, true);
      break;
    default:
      return true;
  }
  copy_add_interval->args[0]= new_date;

  Item *diff;

  if (!date_sub_interval)
  {
    diff=
      new (thd->mem_root) Item_func_timediff(thd,
        (Item *)copy_add_interval, new_date);
  }
  else
  {
    diff=
      new (thd->mem_root) Item_func_timediff(thd,
        new_date, (Item *)copy_add_interval);
  }

  diff->fix_fields(thd, (Item **)diff);
  int interval_val= diff->val_int();

  if (builder->work_with_interval(interval_val, base, subtr,
                                  date_sub_interval))
    return true;

  return false;
}


/**
  Creates a map of non-zero positive vector factors and calculates
  vector rank (count of non-zero vector factors).
*/

void
Ineq_builder::prepare_vector_internal_info(uint vector_idx)
{
  Ineq_vector *vector= get_vector_at(vector_idx);
  uint beg= vector->first_elem_ref;

  for (uint i=0; i < vector_length; i++, beg++)
  {
    Ineq_vector_elem *val= &vector_elements.at(beg);
    if (my_decimal_cmp(&val->initial_value, &null_value))
      vector->positive_map |= get_fields_map(i);
  }

  vector->rank= get_rank(vector->non_zero_map, vector_length);
}


/**
  @brief
    Check if upper and lower bounds for some field don't contradict.

  @param lower_bound_idx  lower bound for some unknown
  @param upper_bound_idx  upper bound for some unknown

  @details
    The method checks if the lower bound for some field is not greater
    than the upper bound for some field if both of them exist.

  @retval true   if there is contradiction
  @retval false  otherwise
*/

bool Ineq_builder::check_non_contradiction_of_restrictions(int lower_bound_idx, 
                                                           int upper_bound_idx)
{
  if (lower_bound_idx == no_field_value ||
      upper_bound_idx == no_field_value)
    return false;

  Ineq_vector *upp_vector= get_vector_at(lower_bound_idx);
  Ineq_vector *low_vector= get_vector_at(upper_bound_idx);

  my_decimal upp_val;
  my_decimal low_val;

  round_decimal(&get_vector_field_factor_pos(upp_vector, 0)->initial_value,
                                             &upp_val);
  round_decimal(&get_vector_field_factor_pos(low_vector, 0)->initial_value,
                                             &low_val);
  my_decimal_neg(&low_val);

  int decimal_cmp= my_decimal_cmp(&upp_val, &low_val);

  if ((decimal_cmp == 0 &&
      ((upp_vector->sign_of_ineq == MORE_SIGN) ||
       (low_vector->sign_of_ineq == MORE_SIGN))) ||
       decimal_cmp == -1)
  {
    cond_value= Item::COND_FALSE;
    return true;
  }
  return false;
}


/**
  @brief
    Check if constant vector doesn't lead to contradicting result

  @param vector  inequality that should be checked

  @details
    The procedure looks if the inequality is of the form:

    const > 0 (1) or const >= 0 (2), where const is some constant.

    If const <= 0 for (1) or const < 0 for (2) than no new inequalities
    can be deduced from this system.

  @retval true   if there is contradiction
  @retval false  otherwise
*/

bool Ineq_builder::check_constant_vector(Ineq_vector *vector)
{
  if (!vector->is_constant(vector_length))
    return false;

  my_decimal *constant_value=
    &get_vector_field_factor_pos(vector, 0)->upper_bound;
  int decimal_cmp= my_decimal_cmp(constant_value, &null_value);

  if (decimal_cmp == -1 ||
      (decimal_cmp == 0 &&
      vector->sign_of_ineq == MORE_SIGN))
  {
    cond_value= Item::COND_FALSE;
    return true;
  }

  refresh_vector(vector);
  return false;
}


/**
  @brief
    Check if inequality is a border for some field

  @param vector  inequality that should be checked

  @details
    The method checks if the considered inequality is of the form:

    a*x + b > 0      (1)

    where 'x' is some field and 'a','b' are constants where 'a'
    is non-zero constant. Such inequality is called border for field x.
    It checks if the considered inequality is an upper or lower
    border for 'x'.
    It also checks if the considered inequality leads to contradicting result
    and WHERE clause is always false.
    E.g.
      The considered inequality: x > 2
      And before it was found that: x < 1
      The considered inequality will lead to contradicting result.

  @retval true   if an error occurs
  @retval false  otherwise
*/

bool Ineq_builder::vector_is_border(uint vector_idx)
{
  int f_numb= 0;
  int prev_bound_numb;
  bool upper_bound= false;
  Ineq_vector *vector= get_vector_at(vector_idx);

  if (vector->non_zero_map == 0)
    return false;

  for (uint i=1; i<vector_length; i++)
  {
    if (map_intersect_with_field(vector->non_zero_map, i))
    {
      if (f_numb == -1)
        f_numb= i;
      else
        return false;
    }
  }

  /*
    Transform vector to (-1)*x + const > 0 or x + const > 0 form
  */
  Ineq_vector_elem *const_value= get_vector_field_factor_pos(vector, 0);
  Ineq_vector_elem *field_value= get_vector_field_factor_pos(vector, f_numb);
  Ineq_vector_elem field_value_abs= init_new_vector_elem();
  vector_elem_abs(&field_value_abs, field_value);
  div_vector_elements(const_value, const_value,
                      &field_value_abs, prec_increment);

  if (decimal_is_neg(&field_value->initial_value))
  {
    /* Current inequality is an upper bound for its field. */
    put_int_in_decimal(-1, &field_value->initial_value);
    prev_bound_numb= field_range[f_numb].upper_bound_ref;
    upper_bound= true;
  }
  else
  {
    put_int_in_decimal(1, &field_value->initial_value);   
    prev_bound_numb= field_range[f_numb].lower_bound_ref;
  }

  ineq_fields_map field_map= get_fields_map(f_numb);
  if (prev_bound_numb == no_field_value)
  {
    if (upper_bound)
    {
      new_upper_bounds &= field_map;
      field_range[f_numb].upper_bound_ref= vector_idx;
    }
    else
    {
      new_lower_bounds &= field_map;
      field_range[f_numb].lower_bound_ref= vector_idx;
    }

    resolved_fields_cnt++;
    if (check_non_contradiction_of_restrictions(
          field_range[f_numb].lower_bound_ref,
          field_range[f_numb].upper_bound_ref))
      return true;
  }
  else
  {
    /*
      Checks if the previous border for the f_numb can be replaced
      by the new one.

      It should be in these cases:

      1. When new and old constant values are positive and
         new is smaller than old.

         E.g.:
         old:  b + 3 > 0   ~>   b > -3
         new:  b + 2 > 0   ~>   b > -2

         old:  -b + 3 > 0   ~>   b < 3
         new:  -b + 2 > 0   ~>   b < 2

      2. When new and old constant values are negative and
         new is smaller than old.

         old:  b - 2 > 0   ~>   b > 2
         new:  b - 3 > 0   ~>   b > 3

         old:  -b - 2 > 0   ~>   b < -2
         new:  -b - 3 > 0   ~>   b < -3

      3. When new constant is negative and old is positive.

         old:  b + 3 > 0   ~>   b > -3
         new:  b - 2 > 0   ~>   b > 2

         old:  -b + 3 > 0   ~>   b < 3
         new:  -b - 2 > 0   ~>   b < -2
    */
    Ineq_vector *prev_vector= get_vector_at(prev_bound_numb);
    Ineq_vector_elem *prev_value= get_vector_field_factor_pos(prev_vector, 0);
    Ineq_vector_elem *new_value= const_value;
    //round_decimal(&prev_value, &prev_value);///???
    //round_decimal(&new_value, &new_value);///???
    bool prev_val_is_bigger= (my_decimal_cmp(&prev_value->initial_value,
                                             &new_value->initial_value) == 1)
      ? true : false;
    bool rewrite= false;

    if (decimal_is_neg(&prev_value->initial_value))
    {
      if (decimal_is_neg(&new_value->initial_value) && prev_val_is_bigger)
        rewrite= true;
    }
    else
    {
      if (!decimal_is_neg(&new_value->initial_value))
      {
        if (prev_val_is_bigger)
          rewrite= true;
      }
      else
        rewrite= true;
    }

    if (rewrite)
    {
      /* Check non-contradiction with new restriction but doesn't save it. */
      if (prev_vector->initial)
      {
        if (upper_bound &&
            check_non_contradiction_of_restrictions(
              field_range[f_numb].lower_bound_ref, top_vector_idx))
          return true;
        else if (!upper_bound &&
                  check_non_contradiction_of_restrictions(
                    top_vector_idx, field_range[f_numb].upper_bound_ref))
          return true;
      }
      else
      {
        copy_vector(prev_vector, vector);
        vector= get_vector_at(prev_bound_numb);
        vector->initial= false;

        if (check_non_contradiction_of_restrictions(
              field_range[f_numb].lower_bound_ref,
              field_range[f_numb].upper_bound_ref))
          return true;

        if (upper_bound)
        {
          new_upper_bounds &= field_map;
          field_range[f_numb].upper_bound_ref= vector_idx;
        }
        else
        {
          new_lower_bounds &= field_map;
          field_range[f_numb].lower_bound_ref= vector_idx;
        }
      }
    }
    refresh_vector(get_vector_at(top_vector_idx));
  }
  return false;
}


/**
  @brief
    Create new inequality through addition of two inequalities

  @param base    inequality to add
  @param ad      inequality to add
  @param f_numb  field index that should be eliminated after the substitution

  @details
    The method takes base inequality and adds it to ad inequality
    to get new inequality where factor before field with f_numb is 0.

    E.g.:

    3*a + 5*b > 0        (1)

    -7*a + 2 > 0         (2)

    where a,b are unknowns.

    Take (1) as a base inequality and than get the coefficient
    so that the coefficient before a field 'a' will be 0 after
    substituting (1) and (2).

    (1) is multiplied on this coefficient (3/7) and the result is:

    7*a + (5/7)*b > 0    (1*)

    Now substitution of (1*) and (2) can be made:

    (5/7)*b + 2 > 0

    This is a new inequality.
*/

void Ineq_builder::new_vector_computation(Ineq_vector *base,
                                          Ineq_vector *ad,
                                          uint f_numb)
{
  Ineq_vector *new_vector= get_vector_at(top_vector_idx);
  copy_vector(new_vector, base);

  Ineq_vector_elem coeff= init_new_vector_elem();
  Ineq_vector_elem *val_base= get_vector_field_factor_pos(base, f_numb);
  Ineq_vector_elem *val_ad= get_vector_field_factor_pos(ad, f_numb);

  div_vector_elements(&coeff, val_ad, val_base, prec_increment);

  if (decimal_is_neg(&coeff.initial_value))
    make_vector_elem_negative(&coeff);

  multiply_vectors(new_vector, new_vector, &coeff);
  fold_vectors(new_vector, ad);
  vector_elem_set_zero(get_vector_field_factor_pos(new_vector, f_numb));
  new_vector->non_zero_map &= get_inverse_map(get_fields_map(f_numb));
}


/**
  @brief
    Try to get new inequality through f_numb field elimination

  @param vector1  inequality to add
  @param vector2  inequality to add
  @param f_numb   field index that should be eliminated

  @details
    The method tries to add vector1 to vector2 and eliminate field
    with field_numb. If the result of addition has less rank than
    its parents it is saved to the end of vectors list.
    Also it is checked if the result of addition is constant inequality
    or is border.

  @retval true   if error occurs
  @retval false  otherwise
*/

bool Ineq_builder::possibility_of_solving_inequalities(Ineq_vector *vector1,
                                                       Ineq_vector *vector2,
                                                       uint f_numb)
{
  /*
    Check if inequalites can be added to eliminate field with number field_numb.
  */
  ineq_fields_map intersect_map1= vector2->get_negative_map() &
                                  vector1->positive_map;
  ineq_fields_map intersect_map2= vector1->get_negative_map() &
                                  vector2->positive_map;

  /*
    There is no intersection between inequalites on this field.
  */
  if ((!map_intersect_with_field(intersect_map1, f_numb)) &&
      (!map_intersect_with_field(intersect_map2, f_numb)))
    return false;

  ineq_fields_map new_map= vector1->positive_map | vector2->positive_map;
  uint max_rank= get_rank(new_map, vector_length);
  /* Check is the created inequality has max_rank at most. */
  if ((max_rank - 1) > ((vector1->rank >= vector2->rank) ?
                        vector1->rank : vector2->rank))
    return false;

  /**
    If the first vector rank is bigger than the second one first vector
    will be multiplied on the computed coefficient in the aim to make
    coefficients before the eliminated field the same.
  */
  if (vector1->rank >= vector2->rank)
    new_vector_computation(vector1, vector2, f_numb);
  else
    new_vector_computation(vector2, vector1, f_numb);

  Ineq_vector *new_vector= get_vector_at(top_vector_idx);

  if (check_constant_vector(new_vector) ||
      vector_is_border(top_vector_idx))
    return true;

  if (get_vector_at(top_vector_idx)->non_zero_map == 0)
    return false;

  prepare_vector_internal_info(top_vector_idx);
  if ((vector1->sign_of_ineq == MORE_OR_EQUAL_SIGN) &&
      (vector2->sign_of_ineq == MORE_OR_EQUAL_SIGN))
    new_vector->sign_of_ineq= MORE_OR_EQUAL_SIGN;
  else
    new_vector->sign_of_ineq= MORE_SIGN;
  new_vector->initial= false;
  top_vector_idx++;
  return false;
}


/**
  Try to eliminate fields through addition of two vectors.
*/

bool Ineq_builder::solve_system()
{
  uint max_rank= 2;
  while (max_rank < vector_length)
  {
    uint f_numb= 1;
    uint vector_count= top_vector_idx;
    while (f_numb < vector_length)
    {
      for (uint i= 0; i < old_top_vector_idx; i++)
      {
        Ineq_vector *vector1= get_vector_at(i);
        if (vector1->rank != max_rank ||
	          !map_intersect_with_field(vector1->non_zero_map, f_numb))
	        continue;

        for (uint j= i + 1; j < vector_count; j++)
        {
          Ineq_vector *vector2= get_vector_at(j);

  	      if (vector1->rank > max_rank ||
	            !map_intersect_with_field(vector2->non_zero_map, f_numb))
	          continue;

          if (possibility_of_solving_inequalities(vector1, vector2, f_numb))
	          return true;
        }
      }
      f_numb++;
    }
    max_rank++;
  }
  return false;
}


/**
  Try to solve together the border for field f_numb
  and other inequalities of the system.
*/

bool Ineq_builder::vector_substitution(int v_numb, uint f_numb,
                                       uint start, uint end)
{
  if (v_numb == no_field_value)
    DBUG_ASSERT(true);

  Ineq_vector *vector= get_vector_at(v_numb);

  for (uint i= start; i < end; i++)
  {
    Ineq_vector *new_vector= get_vector_at(i);
    /*
      Avoid substitution of a*x > 0 and a*x < 0
    */
    if ((new_vector->rank == 1) ||
        (!map_intersect_with_field(new_vector->non_zero_map, f_numb)))
      continue;

    if (possibility_of_solving_inequalities(vector, new_vector, f_numb))
      return true;
  }
  return false;
}


/**
  For each system field try to substitute its upper and lower bounds in
  inequalities from start_idx to end_idx.
*/

bool Ineq_builder::substitute_system_field_borders(ineq_fields_map upper_bounds,
                                                   ineq_fields_map lower_bounds,
                                                   uint start_idx, uint end_idx)
{
  int f_numb= vector_length - 1;
  if (!upper_bounds && !lower_bounds)
    return false;

  if (upper_bounds)
  {
    while (f_numb > 0)
    {
      if (map_intersect_with_field(upper_bounds, f_numb))
      {
        if (vector_substitution(field_range[f_numb].upper_bound_ref,
                                f_numb, start_idx, end_idx))
	        return true;
      }
    }
    f_numb--;
  }

  if (lower_bounds)
  {
    while (f_numb > 0)
    {
      if (map_intersect_with_field(lower_bounds, f_numb))
      {
        if (vector_substitution(field_range[f_numb].lower_bound_ref,
                                f_numb, start_idx, end_idx))
          return true;
      }
      f_numb--;
    }
  }

  return false;
}


/**
  Get new inequalities through substitution of already found borders
  in other inequalities.
*/

bool Ineq_builder::backward_wave(ineq_fields_map *upper_bounds,
                                 ineq_fields_map *lower_bounds)
{
  /* To avoid rewrites. */
  *upper_bounds &= get_inverse_map(new_upper_bounds);
  *lower_bounds &= get_inverse_map(new_lower_bounds);

  /*
    Tries to substitute borders in inequalities from old_idx to new_idx.
  */
  if (old_top_vector_idx!= top_vector_idx)
  {
    if (substitute_system_field_borders(*upper_bounds, *lower_bounds,
                                        old_top_vector_idx, top_vector_idx))
      return true;
  }

  /*
    Second wave - from 0 to new_idx vectors substitute borders received
    after the first wave.
  */
  ineq_fields_map tmp_new_upper_bounds= new_upper_bounds;
  ineq_fields_map tmp_new_lower_bounds= new_lower_bounds;

  while (new_upper_bounds || new_lower_bounds)
  {
    new_upper_bounds= new_lower_bounds= 0;
    if (substitute_system_field_borders(tmp_new_upper_bounds,
                                        tmp_new_lower_bounds,
                                        0, top_vector_idx))
      return true;
    tmp_new_lower_bounds&= new_upper_bounds;
    tmp_new_lower_bounds&= new_lower_bounds;
  }
  /* Add new borders. */
  *upper_bounds&= tmp_new_lower_bounds;
  *lower_bounds&= tmp_new_lower_bounds;

  /* Update the last received vector index. */
  old_top_vector_idx= top_vector_idx;
  return false;
}


bool Ineq_builder::infer_inequalities_for_ineq_system(THD *thd,
                                                    Linear_ineq_system *system)
{
  init_field_structs();

  new_upper_bounds= new_lower_bounds= 0;

  ineq_fields_map upper_bounds;
  ineq_fields_map lower_bounds;

  for (uint i=0; i < top_vector_idx; i++)
  {
    /**
      Precompute check.
      For each inequality in a list check if it is a
      border for some field or is constant (const > 0).
      In this case it can't be used in a new inequality deduction.
    */
    Ineq_vector *new_vector= get_vector_at(i);
    if (new_vector->non_zero_map == 0)
      continue;
    if (check_constant_vector(new_vector) || vector_is_border(i))
      return true;
  }

  old_top_vector_idx= top_vector_idx;
  /*
    First check. Substitute using initial inequalities borders.
  */
  if (backward_wave(&upper_bounds, &lower_bounds))
    return true;

  /*
    (1) - all fields borders are not found
    (2) - new inequalities were added on the previous step
  */
  while (resolved_fields_cnt != 2*vector_length - 2 &&  // (1)
         old_top_vector_idx != top_vector_idx)          // (2)
  {
    if (top_for_new_values == 0)
    {
      if (solve_system())
        return true;
    }
    else
    {
      if (resolved_fields_cnt == 2*vector_length-2)
        break;
    }
    if (backward_wave(&upper_bounds, &lower_bounds))
      return true;
  }

  return false;
}


/**
  Infer inequalities from the WHERE clause linear inequalities.
*/

COND *infer_inequalities(JOIN *join, Item **cond,
                         Item::cond_result *cond_value,
                         int prec_increment)
{
  // arena???
  THD *thd= join->thd;
  Ineq_builder builder(prec_increment, cond_value);

  if (builder.extract_linear_inequalities(thd, *cond))
  {
    *cond_value= builder.cond_value;
    return join->conds;
  }

  builder.sys_it.rewind();
  Linear_ineq_system *system;

  while ((system=builder.sys_it++))
  {
    /*
      Normalize inequalities of the system and interprent them as the subjects
      of Ineq_vector class.

      E.g. 3*a - b < 2 will be transformed into -3*a + b + 2 > 0 and in
      the dynamic array containing sequences of vectors this inequality
      will be introduced by:
      a  b const
      -3  1  -2

      -a + 5*b >= -3 will be transformed into -a + 5*b + 3 >= 0 and in
      the dynamic array containing sequences of vectors this inequality
      will be introduced by:
      a  b const
      -1  5   3

      3*a + b = 5 will be transformed into two inequalities
      3*a + b - 5 >= 0 and -3*a - b + 5 >= 0
      that will be introduced in the dynamic array containing sequences
      of vectors by:
      a  b const   and   a  b const
      -1  5   3          -3 -1   5
    */
    builder.prepare_for_normalization(system);

    List_iterator<Item> it(system->original_conds);
    Item *elem;
    while ((elem=it++))
      elem->walk(&Item::ineq_normalization_processor,
                 0, (uchar *) (&builder));

    uint vector_count= builder.top_vector_idx;
    for (uint j=0; j < vector_count; j++)
      builder.prepare_vector_internal_info(j);

    if (builder.infer_inequalities_for_ineq_system(thd, system))
    {
      *cond_value= builder.cond_value;
      return join->conds;
    }

    if (builder.curr_conds->elements == 0)
      continue;

    builder.cond_value= Item::COND_OK;

    Item_cond_and *new_cond_list=
      new (thd->mem_root) Item_cond_and(thd, *builder.curr_conds);
    thd->change_item_tree(cond,
                          and_conds(thd, *cond, new_cond_list));
    if ((*cond)->fix_fields(thd, 0))
      return join->conds;
  }
  join->cond_equal= &((Item_cond_and *) (join->conds))->m_cond_equal;
  *cond_value= builder.cond_value;

  return (COND *) *cond;
}
