/*
   Copyright (c) 2017, 2020, MariaDB

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

/*
  This file contains functions to support the splitting technique.
  This optimization technique can be applied to equi-joins involving
  materialized tables such as materialized views, materialized derived tables
  and materialized CTEs. The technique also could be applied to materialized
  semi-joins though the code below does not support this usage yet.

  Here are the main ideas behind this technique that we'll call SM optimization
  (SplitMaterialization).

  Consider the query
    SELECT t1.a, t.min
      FROM t1, (SELECT t2.a, MIN(t2.b) as min FROM t2 GROUP BY t2.a) t
    WHERE t1.a = t.a and t1.b < const

  Re-write the query into
    SELECT t1.a, t.min
      FROM t1, LATERAL (SELECT t2.a, MIN(t2.b) as min
                        FROM t2 WHERE t2.a = t1.a GROUP BY t2.a) t
    WHERE t1.b < const

  The execution of the original query (Q1) does the following:
    1. Executes the query in the specification of the derived table
       and puts the result set into a temporary table with an index
       on the first column.
    2. Joins t1 with the temporary table using the its index.

  The execution of the transformed query (Q1R) follows these steps:
    1. For each row of t1 where t1.b < const a temporary table
       containing all rows of of t2 with t2.a = t1.a is created
    2. If there are any rows in the temporary table aggregation
       is performed for them
    3. The result of the aggregation is joined with t1.

  The second execution can win if:
    a) There is an efficient way to select rows of t2 for which t2.a = t1.a
       (For example if there is an index on t2.a)
    and
    b) The number of temporary tables created for partitions
       is much smaller that the total number of partitions

  It should be noted that for the transformed query aggregation
  for a partition may be performed several times.

  As we can see the optimization basically splits table t2 into
  partitions and performs aggregation over each of them
  independently.

  If we have only one equi-join condition then we either push it as
  for Q1R or we don't. In a general case we may have much more options.
  Consider the query (Q3)
    SELECT
      FROM t1,t2 (SELECT t3.a, t3.b, MIN(t3.c) as min
                  FROM t3 GROUP BY a,b) t
    WHERE t.a = t1.a AND t.b = t2.b
          AND t1.c < c1 and t2.c < c2
          AND P(t1,t2);
  (P(t1,t2) designates  some additional conditions over columns of t1,t2).

  Assuming that there indexes on t3(a,b) and t3(b) here we have several
  reasonable options to push equi-join conditions into the derived.
  All these options should be taken into account when the optimizer
  evaluates different join orders. When the join order (t1,t,t2) is
  evaluated there is only one way of splitting : to push the condition
  t.a = t1.a into t. With the join order (t2,t,t1) only the condition
  t.b = t2.b can be pushed. When the join orders (t1,t2,t) and (t2,t1,t)
  are evaluated then the optimizer should consider pushing t.a = t1.a,
  t.b = t2.b and (t.a = t1.a AND t.b = t2.b) to choose the best condition
  for splitting. Apparently here last condition is the best one because
  it provides the miximum possible number of partitions.

  If we dropped the index on t3(a,b) and created the index on t3(a) instead
  then we would have two options for splitting: to push t.a = t1.a or to
  push t.b = t2.b. If the selectivity of the index t3(a) is better than
  the selectivity of t3(b) then the first option is preferred.

  Although the condition (t.a = t1.a AND t.b = t2.b) provides a better
  splitting than the condition t.a = t1.a the latter will be used for
  splitting if the execution plan with the join order (t1,t,t2) turns out
  to be the cheapest one. It's quite possible when the join condition
  P(t1,t2) has a bad selectivity.

  Whenever the optimizer evaluates the cost of using a splitting it
  compares it with the cost of materialization without splitting.

  If we just drop the index on t3(a,b) the chances that the splitting
  will be used becomes much lower but they still exists providing that
  the fanout of the partial join of t1 and t2 is small enough.
*/

/*
  Splitting can be applied to a materialized table specified by the query
  with post-join operations that require partitioning of the result set produced
  by the join expression used in the FROM clause the query such as GROUP BY
  operation and window function operation. In any of these cases the post-join
  operation can be executed independently for any partition only over the rows
  of this partition. Also if the set of all partitions is divided into disjoint
  subsets the operation can applied to each subset independently. In this case
  all rows are first partitioned into the groups each of which contains all the
  rows from the partitions belonging the same subset and then each group
  is subpartitioned into groups in the the post join operation.

  The set of all rows belonging to the union of several partitions is called
  here superpartition. If a grouping operation is defined by the list
  e_1,...,e_n then any set S = {e_i1,...,e_ik} can be used to devide all rows
  into superpartions such that for any two rows r1, r2  the following holds:
  e_ij(r1) = e_ij(r2) for each e_ij from S. We use the splitting technique
  only if S consists of references to colums  of the joined tables.
  For example if the GROUP BY list looks like this a, g(b), c we can consider
  applying the splitting technique to the superpartitions defined by {a,c},
  {a}, {c} (a and c here may be the references to the columns from different
  tables).
*/

 /*
   The following describes when and how the optimizer decides whether it
   makes sense to employ the splitting technique.

   1. For each instance of a materialized table (derived/view/CTE) it is
      checked that it is potentially splittable. Now it is done right after the
      execution plan for the select specifying this table has been chosen.

   2. Any potentially splittable materialized table T is subject to two-phase
      optimization. It means that the optimizer first builds the best execution
      plan for join that specifies T. Then the control is passed back to the
      optimization process of the embedding select Q. After the execution plan
      for Q has been chosen the optimizer finishes the optimization of the join
      specifying T.

   3. When the optimizer builds the container with the KEYUSE structures
      for the join of embedding select it detects the equi-join conditions
      PC that potentially could be pushed into a potentially splittable
      materialized table T. The collected information about such conditions
      is stored together with other facts on potential splittings for table T.

   4. When the optimizer starts looking for the best execution plan for the
      embedding select Q for each potentially splittable materialized table T
      it creates special KEYUSE structures for pushable equi-join conditions
      PC. These structures are used to add new elements to the container
      of KEYUSE structures built for T. The specifics of these elements is
      that they can be ebabled and disabled during the process of choosing
      the best plan for Q.

   5. When the optimizer extends a partial join order with a potentially
      splittable materialized table T (in function best_access_path) it
      first evaluates a new execution plan for the modified specification
      of T that adds all equi-join conditions that can be pushed with
      current join prefix to the WHERE conditions of the original
      specification of T. If the cost of the new plan is better than the
      the cost of the original materialized table then the optimizer
      prefers to use splitting for the current join prefix. As the cost
      of the plan depends only on the pushed conditions it makes sense
      to cache this plan for other prefixes.

   6. The optimizer takes into account the cost of splitting / materialization
      of a potentially splittable materialized table T as a startup cost
      to access table T.

   7. When the optimizer finally chooses the best execution plan for
      the embedding select Q and this plan prefers using splitting
      for table T with pushed equi-join conditions PC then the execution
      plan for the underlying join with these conditions is chosen for T.
*/

/*
  The implementation of the splitting technique below allows to apply
  the technique only to a materialized derived table / view / CTE whose
  specification is either a select with GROUP BY or a non-grouping select
  with window functions that share the same PARTITION BY list.
*/

#include "mariadb.h"
#include "sql_select.h"
#include "opt_trace.h"

/* Info on a splitting field */
struct SplM_field_info
{
  /* Splitting field in the materialized table T */
  Field *mat_field;
  /* The item from the select list of the specification of T */
  Item *producing_item;
  /* The corresponding splitting field from the specification of T */
  Field *underlying_field;
};


/* Info on the splitting execution plan saved in SplM_opt_info::cache */
struct SplM_plan_info
{
  /* The cached splitting execution plan P */
  POSITION *best_positions;
  /* The cost of the above plan */
  double cost;
  /* Selectivity of splitting used in P */
  double split_sel;
  /* For fast search of KEYUSE_EXT elements used for splitting in P */
  struct KEYUSE_EXT *keyuse_ext_start;
  /* The tables that contains the fields used for splitting in P */
  TABLE *table;
  /* The number of the key from 'table' used for splitting in P */
  uint key;
  /* Number of the components of 'key' used for splitting in P */
  uint parts;
};


/*
  The structure contains the information that is used by the optimizer
  for potentially splittable materialization of T  that is a materialized
  derived_table / view / CTE
*/
class SplM_opt_info : public Sql_alloc
{
public:
  /* The join for the select specifying T */
  JOIN *join;
  /* The map of tables from 'join' whose columns can be used for partitioning */  
  table_map tables_usable_for_splitting;
  /* Info about the fields of the joined tables usable for splitting */
  SplM_field_info *spl_fields;
  /* The number of elements in the above list */
  uint spl_field_cnt;
  /* The list of equalities injected into WHERE for split optimization */
  List<Item> inj_cond_list;
  /* Contains the structures to generate all KEYUSEs for pushable equalities */
  List<KEY_FIELD> added_key_fields;
  /* The cache of evaluated execution plans for 'join' with pushed equalities */
  List<SplM_plan_info> plan_cache;
  /* Cost of best execution plan for join when nothing is pushed */
  double unsplit_cost;
  /* Cardinality of T when nothing is pushed */
  double unsplit_card;
  /* Lastly evaluated execution plan for 'join' with pushed equalities */
  SplM_plan_info *last_plan;

  SplM_plan_info *find_plan(TABLE *table, uint key, uint parts);
};


void TABLE::set_spl_opt_info(SplM_opt_info *spl_info)
{
  if (spl_info)
    spl_info->join->spl_opt_info= spl_info;
  spl_opt_info= spl_info;
}


void TABLE::deny_splitting()
{
  DBUG_ASSERT(spl_opt_info != NULL);
  spl_opt_info->join->spl_opt_info= NULL;
  spl_opt_info= NULL;
}


double TABLE::get_materialization_cost()
{
  DBUG_ASSERT(spl_opt_info != NULL);
  return spl_opt_info->unsplit_cost;
}


/* This structure is auxiliary and used only in the function that follows it */
struct SplM_field_ext_info: public SplM_field_info
{
  uint item_no;
  bool is_usable_for_ref_access;
};


/**
  @brief
    Check whether this join is one for potentially splittable materialized table

  @details
    The function checks whether this join is for select that specifies
    a potentially splittable materialized table T. If so, the collected
    info on potential splittability of T is attached to the field spl_opt_info
    of the TABLE structure for T.

    The function returns a positive answer if the following holds:
    1. the optimizer switch 'split_materialized' is set 'on'
    2. the select owning this join specifies a materialized derived/view/cte T
    3. this is the only select in the specification of T
    4. condition pushdown is not prohibited into T
    5. T is not recursive
    6. not all of this join are constant or optimized away
    7. T is either
       7.1. a grouping table with GROUP BY list P
       or
       7.2. a non-grouping table with window functions over the same non-empty
            partition specified by the PARTITION BY list P
    8. P contains some references on the columns of the joined tables C
       occurred also in the select list of this join
    9. There are defined some keys usable for ref access of fields from C
       with available statistics.
    10. The select doesn't use WITH ROLLUP (This limitation can probably be
       lifted)

  @retval
    true   if the answer is positive
    false  otherwise
*/

bool JOIN::check_for_splittable_materialized()
{
  ORDER *partition_list= 0;
  st_select_lex_unit *unit= select_lex->master_unit();
  TABLE_LIST *derived= unit->derived;
  if (!(optimizer_flag(thd, OPTIMIZER_SWITCH_SPLIT_MATERIALIZED)) ||  // !(1)
      !(derived && derived->is_materialized_derived()) ||             // !(2)
      (unit->first_select()->next_select()) ||                        // !(3)
      (derived->prohibit_cond_pushdown) ||                            // !(4)
      (derived->is_recursive_with_table()) ||                         // !(5)
      (table_count == 0 || const_tables == top_join_tab_count) ||     // !(6)
      rollup.state != ROLLUP::STATE_NONE)                             // (10)
    return false;
  if (group_list)                                                     // (7.1)
  {
    if (!select_lex->have_window_funcs())
      partition_list= group_list;
  }
  else if (select_lex->have_window_funcs() &&
           select_lex->window_specs.elements == 1)                    // (7.2)
  {
    partition_list=
      select_lex->window_specs.head()->partition_list->first;
  }
  if (!partition_list)
    return false;

  ORDER *ord;
  Dynamic_array<SplM_field_ext_info> candidates(PSI_INSTRUMENT_MEM);

  /*
    Select from partition_list all candidates for splitting.
    A candidate must be
    - field item or refer to such (8.1)
    - item mentioned in the select list (8.2)
    Put info about such candidates into the array candidates
  */
  table_map usable_tables= 0;  // tables that contains the candidate
  for (ord= partition_list; ord; ord= ord->next)
  {
    Item *ord_item= *ord->item;
    if (ord_item->real_item()->type() != Item::FIELD_ITEM)   // !(8.1)
      continue;

    Field *ord_field= ((Item_field *) (ord_item->real_item()))->field;

    /* Ignore fields from  of inner tables of outer joins */
    TABLE_LIST *tbl= ord_field->table->pos_in_table_list;
    if (tbl->is_inner_table_of_outer_join())
      continue;

    List_iterator<Item> li(fields_list);
    Item *item;
    uint item_no= 0;
    while ((item= li++))
    {
      if ((*ord->item)->eq(item, 0))       // (8.2)
      {
	SplM_field_ext_info new_elem;
        new_elem.producing_item= item;
        new_elem.item_no= item_no;
        new_elem.mat_field= derived->table->field[item_no];
        new_elem.underlying_field= ord_field;
        new_elem.is_usable_for_ref_access= false;
        candidates.push(new_elem);
        usable_tables|= ord_field->table->map;
        break;
      }
      item_no++;
    }
  }
  if (candidates.elements() == 0)  // no candidates satisfying (8.1) && (8.2)
    return false;

  /*
    For each table from this join find the keys that can be used for ref access
    of the fields mentioned in the 'array candidates'
  */

  SplM_field_ext_info *cand;
  SplM_field_ext_info *cand_start= &candidates.at(0);
  SplM_field_ext_info *cand_end= cand_start + candidates.elements();

  for (JOIN_TAB *tab= join_tab;
       tab < join_tab + top_join_tab_count; tab++)
  {
    TABLE *table= tab->table;
    if (!(table->map & usable_tables))
      continue;

    table->keys_usable_for_splitting.clear_all();
    uint i;
    for (i= 0; i < table->s->keys; i++)
    {
      if (!table->keys_in_use_for_query.is_set(i))
        continue;
      KEY *key_info= table->key_info + i;
      uint key_parts= table->actual_n_key_parts(key_info);
      uint usable_kp_cnt= 0;
      for ( ; usable_kp_cnt < key_parts; usable_kp_cnt++)
      {
        if (key_info->actual_rec_per_key(usable_kp_cnt) == 0)
          break;
        int fldnr= key_info->key_part[usable_kp_cnt].fieldnr;

        for (cand= cand_start; cand < cand_end; cand++)
        {
          if (cand->underlying_field->table == table &&
              cand->underlying_field->field_index + 1 == fldnr)
	  {
            cand->is_usable_for_ref_access= true;
            break;
          }
        }
        if (cand == cand_end)
          break;
      }
      if (usable_kp_cnt)
        table->keys_usable_for_splitting.set_bit(i);
    }
  }

  /* Count the candidate fields that can be accessed by ref */
  uint spl_field_cnt= (uint)candidates.elements();
  for (cand= cand_start; cand < cand_end; cand++)
  {
    if (!cand->is_usable_for_ref_access)
      spl_field_cnt--;
  }

  if (!spl_field_cnt)  // No candidate field can be accessed by ref => !(9)
    return false;

  /*
    Create a structure of the type SplM_opt_info and fill it with
    the collected info on potential splittability of T
  */
  SplM_opt_info *spl_opt_info= new (thd->mem_root) SplM_opt_info();
  SplM_field_info *spl_field=
    (SplM_field_info *) (thd->calloc(sizeof(SplM_field_info) *
                                            spl_field_cnt));

  if (!(spl_opt_info && spl_field)) // consider T as not good for splitting
    return false;

  spl_opt_info->join= this;
  spl_opt_info->tables_usable_for_splitting= 0;
  spl_opt_info->spl_field_cnt= spl_field_cnt;
  spl_opt_info->spl_fields= spl_field;
  for (cand= cand_start; cand < cand_end; cand++)
  {
    if (!cand->is_usable_for_ref_access)
      continue;
    spl_field->producing_item= cand->producing_item;
    spl_field->underlying_field= cand->underlying_field;
    spl_field->mat_field= cand->mat_field;
    spl_opt_info->tables_usable_for_splitting|=
    cand->underlying_field->table->map;
    spl_field++;
  }

  /* Attach this info to the table T */
  derived->table->set_spl_opt_info(spl_opt_info);

  /*
    If this is specification of a materialized derived table T that is
    potentially splittable and is used in the from list of the right operand
    of an IN predicand transformed to a semi-join then the embedding semi-join
    nest is not allowed to be materialized.
  */
  if (derived && derived->is_materialized_derived() &&
      derived->embedding && derived->embedding->sj_subq_pred)
    derived->embedding->sj_subq_pred->types_allow_materialization= FALSE;
  return true;
}


/**
  @brief
    Collect info on KEY_FIELD usable for splitting

  @param
    key_field     KEY_FIELD to collect info on

  @details
    The function assumes that this table is potentially splittable.
    The function checks whether the KEY_FIELD structure key_field built for
    this  table was created for a splitting field f. If so, the function does
    the following using info from key_field:
    1. Builds an equality of the form f = key_field->val that could be
       pushed into this table.
    2. Creates a new KEY_FIELD structure for this equality and stores
       a reference to this structure in this->spl_opt_info.
*/

void TABLE::add_splitting_info_for_key_field(KEY_FIELD *key_field)
{
  DBUG_ASSERT(spl_opt_info != NULL);
  JOIN *join= spl_opt_info->join;
  Field *field= key_field->field;
  SplM_field_info *spl_field= spl_opt_info->spl_fields;
  uint i= spl_opt_info->spl_field_cnt;
  for ( ; i; i--, spl_field++)
  {
    if (spl_field->mat_field == field)
      break;
  }
  if (!i)         // field is not usable for splitting
    return;

  /*
    Any equality condition that can be potentially pushed into the
    materialized derived table is constructed now though later it may turn out
    that it is not needed, because it is not used for splitting.
    The reason for this is that the failure to construct it when it has to be
    injected causes denial for further processing of the query.
    Formally this equality is needed in the KEY_FIELD structure constructed
    here that will be used to generate additional keyuses usable for splitting.
    However key_field.cond could be used for this purpose (see implementations
    of virtual function can_optimize_keypart_ref()).

    The condition is built in such a form that it can be added to the WHERE
    condition of the select that specifies this table.
  */
  THD *thd= in_use;
  Item *left_item= spl_field->producing_item->build_clone(thd);
  Item *right_item= key_field->val->build_clone(thd);
  Item_func_eq *eq_item= 0;
  if (left_item && right_item)
  {
    right_item->walk(&Item::set_fields_as_dependent_processor,
                     false, join->select_lex);
    right_item->update_used_tables();
    eq_item= new (thd->mem_root) Item_func_eq(thd, left_item, right_item);
  }
  if (!eq_item)
    return;
  KEY_FIELD *added_key_field=
    (KEY_FIELD *) thd->alloc(sizeof(KEY_FIELD));
  if (!added_key_field ||
      spl_opt_info->added_key_fields.push_back(added_key_field,thd->mem_root))
    return;
  added_key_field->field= spl_field->underlying_field;
  added_key_field->cond= eq_item;
  added_key_field->val= key_field->val;
  added_key_field->level= 0;
  added_key_field->optimize= KEY_OPTIMIZE_EQ;
  added_key_field->eq_func= true;

  Item *real= key_field->val->real_item();
  if ((real->type() == Item::FIELD_ITEM) &&
        ((Item_field*)real)->field->maybe_null())
    added_key_field->null_rejecting= true;
  else
    added_key_field->null_rejecting= false;

  added_key_field->cond_guard= NULL;
  added_key_field->sj_pred_no= UINT_MAX;
  return;
}


static bool
add_ext_keyuse_for_splitting(Dynamic_array<KEYUSE_EXT> *ext_keyuses,
                             KEY_FIELD *added_key_field, uint key, uint part)
{
  KEYUSE_EXT keyuse_ext;
  Field *field= added_key_field->field;

  JOIN_TAB *tab=field->table->reginfo.join_tab;
  key_map possible_keys=field->get_possible_keys();
  possible_keys.intersect(field->table->keys_usable_for_splitting);
  tab->keys.merge(possible_keys);

  Item_func_eq *eq_item= (Item_func_eq *) (added_key_field->cond);
  keyuse_ext.table= field->table;
  keyuse_ext.val= eq_item->arguments()[1];
  keyuse_ext.key= key;
  keyuse_ext.keypart=part;
  keyuse_ext.keypart_map= (key_part_map) 1 << part;
  keyuse_ext.used_tables= keyuse_ext.val->used_tables();
  keyuse_ext.optimize= added_key_field->optimize & KEY_OPTIMIZE_REF_OR_NULL;
  keyuse_ext.ref_table_rows= 0;
  keyuse_ext.null_rejecting= added_key_field->null_rejecting;
  keyuse_ext.cond_guard= added_key_field->cond_guard;
  keyuse_ext.sj_pred_no= added_key_field->sj_pred_no;
  keyuse_ext.validity_ref= 0;
  keyuse_ext.needed_in_prefix= added_key_field->val->used_tables();
  keyuse_ext.validity_var= false;
  return ext_keyuses->push(keyuse_ext);
}


static int
sort_ext_keyuse(KEYUSE_EXT *a, KEYUSE_EXT *b)
{
  if (a->table->tablenr != b->table->tablenr)
    return (int) (a->table->tablenr - b->table->tablenr);
  if (a->key != b->key)
    return (int) (a->key - b->key);
  return (int) (a->keypart - b->keypart);
}


static void
sort_ext_keyuses(Dynamic_array<KEYUSE_EXT> *keyuses)
{
  KEYUSE_EXT *first_keyuse= &keyuses->at(0);
  my_qsort(first_keyuse, keyuses->elements(), sizeof(KEYUSE_EXT),
           (qsort_cmp) sort_ext_keyuse);
}


/**
  @brief
    Add info on keyuses usable for splitting into an array
*/

static bool
add_ext_keyuses_for_splitting_field(Dynamic_array<KEYUSE_EXT> *ext_keyuses,
                                    KEY_FIELD *added_key_field)
{
  Field *field= added_key_field->field;
  TABLE *table= field->table;
  for (uint key= 0; key < table->s->keys; key++)
  {
    if (!(table->keys_usable_for_splitting.is_set(key)))
      continue;
    KEY *key_info= table->key_info + key;
    uint key_parts= table->actual_n_key_parts(key_info);
    KEY_PART_INFO *key_part_info= key_info->key_part;
    for (uint part=0; part <  key_parts; part++, key_part_info++)
    {
      if (!field->eq(key_part_info->field))
        continue;
      if (add_ext_keyuse_for_splitting(ext_keyuses, added_key_field, key, part))
        return true;
    }
  }
  return false;
}


/*
  @brief
    Cost of the post join operation used in specification of splittable table
*/

static
double spl_postjoin_oper_cost(THD *thd, double join_record_count, uint rec_len)
{
  double cost;
  cost=  get_tmp_table_write_cost(thd, join_record_count,rec_len) *
         join_record_count;   // cost to fill tmp table
  cost+= get_tmp_table_lookup_cost(thd, join_record_count,rec_len) *
         join_record_count;   // cost to perform post join operation used here
  cost+= get_tmp_table_lookup_cost(thd, join_record_count, rec_len) +
         (join_record_count == 0 ? 0 :
          join_record_count * log2 (join_record_count)) *
         SORT_INDEX_CMP_COST;             // cost to perform  sorting
  return cost;
}

/**
  @brief
    Add KEYUSE structures that can be usable for splitting

  @details
    This function is called only for joins created for potentially
    splittable materialized tables. The function does the following:
    1. Creates the dynamic array ext_keyuses_for_splitting of KEYUSE_EXT
       structures and fills is with info about all keyuses that
       could be used for splitting.
    2. Sort the array ext_keyuses_for_splitting for fast access by key
       on certain columns.
    3. Collects and stores cost and cardinality info on the best execution
       plan that does not use splitting and save this plan together with
       corresponding array of keyuses.
    4. Expand this array with KEYUSE elements built from the info stored
       in ext_keyuses_for_splitting that could be produced by pushed
       equalities employed for splitting.
    5. Prepare the extended array of keyuses to be used in the function
       best_access_plan()
*/

void JOIN::add_keyuses_for_splitting()
{
  uint i;
  size_t idx;
  KEYUSE_EXT *keyuse_ext;
  KEYUSE_EXT keyuse_ext_end;
  double oper_cost;
  uint rec_len;
  uint added_keyuse_count;
  TABLE *table= select_lex->master_unit()->derived->table;
  List_iterator_fast<KEY_FIELD> li(spl_opt_info->added_key_fields);
  KEY_FIELD *added_key_field;
  if (!spl_opt_info->added_key_fields.elements)
    goto err;
  if (!(ext_keyuses_for_splitting= new Dynamic_array<KEYUSE_EXT>(PSI_INSTRUMENT_MEM)))
    goto err;
  while ((added_key_field= li++))
  {
    (void) add_ext_keyuses_for_splitting_field(ext_keyuses_for_splitting,
                                               added_key_field);
  }
  added_keyuse_count= (uint)ext_keyuses_for_splitting->elements();
  if (!added_keyuse_count)
    goto err;
  sort_ext_keyuses(ext_keyuses_for_splitting);
  bzero((char*) &keyuse_ext_end, sizeof(keyuse_ext_end));
  if (ext_keyuses_for_splitting->push(keyuse_ext_end))
    goto err;

  spl_opt_info->unsplit_card= join_record_count;

  rec_len= table->s->rec_buff_length;

  oper_cost= spl_postjoin_oper_cost(thd, join_record_count, rec_len);

  spl_opt_info->unsplit_cost= best_positions[table_count-1].read_time +
                              oper_cost;

  if (!(save_qep= new Join_plan_state(table_count + 1)))
    goto err;

  save_query_plan(save_qep);

  if (!keyuse.buffer &&
       my_init_dynamic_array(PSI_INSTRUMENT_ME, &keyuse, sizeof(KEYUSE),
                             20, 64, MYF(MY_THREAD_SPECIFIC)))
    goto err;

  if (allocate_dynamic(&keyuse, save_qep->keyuse.elements + added_keyuse_count))
    goto err;

  idx= keyuse.elements= save_qep->keyuse.elements;
  if (keyuse.elements)
    memcpy(keyuse.buffer,
           save_qep->keyuse.buffer,
           (size_t) keyuse.elements * keyuse.size_of_element);

  keyuse_ext= &ext_keyuses_for_splitting->at(0);
  for (i=0; i < added_keyuse_count; i++, keyuse_ext++, idx++)
  {
    set_dynamic(&keyuse, (KEYUSE *) keyuse_ext, idx);
    KEYUSE *added_keyuse= ((KEYUSE *) (keyuse.buffer)) + idx;
    added_keyuse->validity_ref= &keyuse_ext->validity_var;
  }

  if (sort_and_filter_keyuse(thd, &keyuse, true))
    goto err;
  optimize_keyuse(this, &keyuse);

  for (uint i= 0; i < table_count; i++)
  {
    JOIN_TAB *tab= join_tab + i;
    map2table[tab->table->tablenr]= tab;
  }

  return;

err:
  if (save_qep)
    restore_query_plan(save_qep);
  table->deny_splitting();
  return;
}


/**
  @brief
    Add KEYUSE structures that can be usable for splitting of this joined table
*/

void JOIN_TAB::add_keyuses_for_splitting()
{
  DBUG_ASSERT(table->spl_opt_info != NULL);
  SplM_opt_info *spl_opt_info= table->spl_opt_info;
  spl_opt_info->join->add_keyuses_for_splitting();
}


/*
  @brief
     Find info on the splitting plan by the splitting key
*/

SplM_plan_info *SplM_opt_info::find_plan(TABLE *table, uint key, uint parts)
{
  List_iterator_fast<SplM_plan_info> li(plan_cache);
  SplM_plan_info *spl_plan;
  while ((spl_plan= li++))
  {
    if (spl_plan->table == table &&
        spl_plan->key == key &&
        spl_plan->parts == parts)
      break;
  }
  return spl_plan;
}


/*
  @breaf
    Enable/Disable a keyuses that can be used for splitting
 */

static
void reset_validity_vars_for_keyuses(KEYUSE_EXT *key_keyuse_ext_start,
                                     TABLE *table, uint key,
                                     table_map remaining_tables,
                                     bool validity_val)
{
  KEYUSE_EXT *keyuse_ext= key_keyuse_ext_start;
  do
  {
    if (!(keyuse_ext->needed_in_prefix & remaining_tables))
    {
      /*
        The enabling/disabling flags are set just in KEYUSE_EXT structures.
        Yet keyuses that are used by best_access_path() have pointers
        to these flags.
      */
      keyuse_ext->validity_var= validity_val;
    }
    keyuse_ext++;
  }
  while (keyuse_ext->key == key && keyuse_ext->table == table);
}


/**
  @brief
    Choose the best splitting to extend the evaluated partial join

  @param
    record_count      estimated cardinality of the extended partial join
    remaining_tables  tables not joined yet

  @details
    This function is called during the search for the best execution
    plan of the join that contains this table T. The function is called
    every time when the optimizer tries to extend a partial join by
    joining it with table T. Depending on what tables are already in the
    partial join different equalities usable for splitting can be pushed
    into T. The function evaluates different variants and chooses the
    best one. Then the function finds the plan for the materializing join
    with the chosen equality conditions pushed into it. If the cost of the
    plan turns out to be less than the cost of the best plan without
    splitting the function set it as the true plan of materialization
    of the table T.
    The function caches the found plans for materialization of table T
    together if the info what key was used for splitting. Next time when
    the optimizer prefers to use the same key the plan is taken from
    the cache of plans

  @retval
    Pointer to the info on the found plan that employs the pushed equalities
    if the plan has been chosen, NULL - otherwise.
*/

SplM_plan_info * JOIN_TAB::choose_best_splitting(double record_count,
                                                 table_map remaining_tables)
{
  SplM_opt_info *spl_opt_info= table->spl_opt_info;
  DBUG_ASSERT(spl_opt_info != NULL);
  JOIN *join= spl_opt_info->join;
  THD *thd= join->thd;
  table_map tables_usable_for_splitting=
              spl_opt_info->tables_usable_for_splitting;
  KEYUSE_EXT *keyuse_ext= &join->ext_keyuses_for_splitting->at(0);
  KEYUSE_EXT *UNINIT_VAR(best_key_keyuse_ext_start);
  TABLE *best_table= 0;
  double best_rec_per_key= DBL_MAX;
  SplM_plan_info *spl_plan= 0;
  uint best_key= 0;
  uint best_key_parts= 0;

  /*
    Check whether there are keys that can be used to join T employing splitting
    and if so, select the best out of such keys
  */
  for (uint tablenr= 0; tablenr < join->table_count; tablenr++)
  {
    if (!((1ULL << tablenr) & tables_usable_for_splitting))
      continue;
    JOIN_TAB *tab= join->map2table[tablenr];
    TABLE *table= tab->table;
    if (keyuse_ext->table != table)
      continue;
    do
    {
      uint key= keyuse_ext->key;
      KEYUSE_EXT *key_keyuse_ext_start= keyuse_ext;
      key_part_map found_parts= 0;
      do
      {
        if (keyuse_ext->needed_in_prefix & remaining_tables)
	{
          keyuse_ext++;
          continue;
        }
        if (!(keyuse_ext->keypart_map & found_parts))
	{
          if ((!found_parts && !keyuse_ext->keypart) ||
              (found_parts && ((keyuse_ext->keypart_map >> 1) & found_parts)))
            found_parts|= keyuse_ext->keypart_map;
          else
	  {
            do
	    {
              keyuse_ext++;
            }
            while (keyuse_ext->key == key && keyuse_ext->table == table);
            break;
          }
        }
        KEY *key_info= table->key_info + key;
        double rec_per_key=
                 key_info->actual_rec_per_key(keyuse_ext->keypart);
        if (rec_per_key < best_rec_per_key)
	{
          best_table= keyuse_ext->table;
          best_key= keyuse_ext->key;
	  best_key_parts= keyuse_ext->keypart + 1;
          best_rec_per_key= rec_per_key;
          best_key_keyuse_ext_start= key_keyuse_ext_start;
        }
        keyuse_ext++;
      }
      while (keyuse_ext->key == key && keyuse_ext->table == table);
    }
    while (keyuse_ext->table == table);
  }
  spl_opt_info->last_plan= 0;
  if (best_table)
  {
    /*
      The key for splitting was chosen, look for the plan for this key
      in the cache
    */
    Json_writer_array spl_trace(thd, "choose_best_splitting");
    spl_plan= spl_opt_info->find_plan(best_table, best_key, best_key_parts);
    if (!spl_plan)
    {
      /*
        The plan for the chosen key has not been found in the cache.
        Build a new plan and save info on it in the cache
      */
      table_map all_table_map= (((table_map) 1) << join->table_count) - 1;
      reset_validity_vars_for_keyuses(best_key_keyuse_ext_start, best_table,
                                      best_key, remaining_tables, true);
      choose_plan(join, all_table_map & ~join->const_table_map);

      /*
        Check that the chosen plan is really a splitting plan.
        If not or if there is not enough memory to save the plan in the cache
        then just return with no splitting plan.
      */
      POSITION *first_non_const_pos= join->best_positions + join->const_tables;
      TABLE *table= first_non_const_pos->table->table;
      key_map spl_keys= table->keys_usable_for_splitting;
      if (!(first_non_const_pos->key &&
            spl_keys.is_set(first_non_const_pos->key->key)) ||
          !(spl_plan= (SplM_plan_info *) thd->alloc(sizeof(SplM_plan_info))) ||
	  !(spl_plan->best_positions=
	     (POSITION *) thd->alloc(sizeof(POSITION) * join->table_count)) ||
	  spl_opt_info->plan_cache.push_back(spl_plan))
      {
        reset_validity_vars_for_keyuses(best_key_keyuse_ext_start, best_table,
                                        best_key, remaining_tables, false);
        return 0;
      }

      spl_plan->keyuse_ext_start= best_key_keyuse_ext_start;
      spl_plan->table= best_table;
      spl_plan->key= best_key;
      spl_plan->parts= best_key_parts;
      spl_plan->split_sel= best_rec_per_key /
                           (spl_opt_info->unsplit_card ?
                            spl_opt_info->unsplit_card : 1); 

      uint rec_len= table->s->rec_buff_length;

      double split_card= spl_opt_info->unsplit_card * spl_plan->split_sel;
      double oper_cost= split_card *
                        spl_postjoin_oper_cost(thd, split_card, rec_len);
      spl_plan->cost= join->best_positions[join->table_count-1].read_time +
                      + oper_cost;

      if (unlikely(thd->trace_started()))
      {
        Json_writer_object wrapper(thd);
        Json_writer_object find_trace(thd, "best_splitting");
        find_trace.add("table", best_table->alias.c_ptr());
        find_trace.add("key", best_table->key_info[best_key].name);
        find_trace.add("record_count", record_count);
        find_trace.add("cost", spl_plan->cost);
        find_trace.add("unsplit_cost", spl_opt_info->unsplit_cost);
      }
      memcpy((char *) spl_plan->best_positions,
             (char *) join->best_positions,
             sizeof(POSITION) * join->table_count);
      reset_validity_vars_for_keyuses(best_key_keyuse_ext_start, best_table,
                                      best_key, remaining_tables, false);
    }
    if (spl_plan)
    {
      if(record_count * spl_plan->cost < spl_opt_info->unsplit_cost - 0.01)
      {
        /*
          The best plan that employs splitting is cheaper than
          the plan without splitting
	*/
        spl_opt_info->last_plan= spl_plan;
      }
    }
  }

  /* Set the cost of the preferred materialization for this partial join */
  records= (ha_rows)spl_opt_info->unsplit_card;
  spl_plan= spl_opt_info->last_plan;
  if (spl_plan)
  {
    startup_cost= record_count * spl_plan->cost;
    records= (ha_rows) (records * spl_plan->split_sel);

    Json_writer_object trace(thd, "lateral_derived");
    trace.add("startup_cost", startup_cost);
    trace.add("splitting_cost", spl_plan->cost);
    trace.add("records", records);
  }
  else
    startup_cost= spl_opt_info->unsplit_cost;
  return spl_plan;
}


/**
  @brief
    Inject equalities for splitting used by the materialization join

  @param
    excluded_tables  used to filter out the equalities that cannot
                      be pushed.

  @details
    This function injects equalities pushed into a derived table T for which
    the split optimization has been chosen by the optimizer. The function
    is called by JOIN::inject_splitting_cond_for_all_tables_with_split_op().
    All equalities usable for splitting T whose right parts do not depend on
    any of the 'excluded_tables' can be pushed into the where clause of the
    derived table T.
    The function also marks the select that specifies T as
    UNCACHEABLE_DEPENDENT_INJECTED.

  @retval
    false  on success
    true   on failure
*/

bool JOIN::inject_best_splitting_cond(table_map excluded_tables)
{
  Item *inj_cond= 0;
  List<Item> *inj_cond_list= &spl_opt_info->inj_cond_list;
  List_iterator<KEY_FIELD> li(spl_opt_info->added_key_fields);
  KEY_FIELD *added_key_field;
  while ((added_key_field= li++))
  {
    if (excluded_tables & added_key_field->val->used_tables())
      continue;
    if (inj_cond_list->push_back(added_key_field->cond, thd->mem_root))
      return true;
  }
  DBUG_ASSERT(inj_cond_list->elements);
  switch (inj_cond_list->elements) {
  case 1:
    inj_cond= inj_cond_list->head(); break;
  default:
    inj_cond= new (thd->mem_root) Item_cond_and(thd, *inj_cond_list);
    if (!inj_cond)
      return true;
  }
  if (inj_cond)
    inj_cond->fix_fields(thd,0);

  if (inject_cond_into_where(inj_cond->copy_andor_structure(thd)))
    return true;

  select_lex->uncacheable|= UNCACHEABLE_DEPENDENT_INJECTED;
  st_select_lex_unit *unit= select_lex->master_unit();
  unit->uncacheable|= UNCACHEABLE_DEPENDENT_INJECTED;

  return false;
}


/**
  @brief
    Test if equality is injected for split optimization

  @param
    eq_item   equality to to test

  @retval
    true    eq_item is equality injected for split optimization
    false   otherwise
*/

bool is_eq_cond_injected_for_split_opt(Item_func_eq *eq_item)
{
  Item *left_item= eq_item->arguments()[0]->real_item();
  if (left_item->type() != Item::FIELD_ITEM)
    return false;
  Field *field= ((Item_field *) left_item)->field;
  if (!field->table->reginfo.join_tab)
    return false;
  JOIN *join= field->table->reginfo.join_tab->join;
  if (!join->spl_opt_info)
    return false;
  List_iterator_fast<Item> li(join->spl_opt_info->inj_cond_list);
  Item *item;
  while ((item= li++))
  {
    if (item == eq_item)
        return true;
  }
  return false;
}


/**
  @brief
    Fix the splitting chosen for a splittable table in the final query plan

  @param
    spl_plan   info on the splitting plan chosen for the splittable table T
    remaining_tables  the table T is joined just before these tables
    is_const_table    the table T is a constant table

  @details
    If in the final query plan the optimizer has chosen a splitting plan
    then the function sets this plan as the final execution plan to
    materialized the table T. Otherwise the plan that does not use
    splitting is set for the materialization.

  @retval
    false  on success
    true   on failure
*/

bool JOIN_TAB::fix_splitting(SplM_plan_info *spl_plan,
                             table_map remaining_tables,
                             bool is_const_table)
{
  SplM_opt_info *spl_opt_info= table->spl_opt_info;
  DBUG_ASSERT(table->spl_opt_info != 0);
  JOIN *md_join= spl_opt_info->join;
  if (spl_plan && !is_const_table)
  {
    memcpy((char *) md_join->best_positions,
           (char *) spl_plan->best_positions,
           sizeof(POSITION) * md_join->table_count);
    /*
      This is called for a proper work of JOIN::get_best_combination()
      called for the join that materializes T
    */
    reset_validity_vars_for_keyuses(spl_plan->keyuse_ext_start,
                                    spl_plan->table,
                                    spl_plan->key,
                                    remaining_tables,
                                    true);
  }
  else if (md_join->save_qep)
  {
    md_join->restore_query_plan(md_join->save_qep);
  }
  return false;
}


/**
  @brief
    Fix the splittings chosen splittable tables in the final query plan

  @details
    The function calls JOIN_TAB::fix_splittins for all potentially
    splittable tables in this join to set all final materialization
    plans chosen for these tables.

  @retval
    false  on success
    true   on failure
*/

bool JOIN::fix_all_splittings_in_plan()
{
  table_map prev_tables= 0;
  table_map all_tables= (table_map(1) << table_count) - 1;
  for (uint tablenr= 0; tablenr < table_count; tablenr++)
  {
    POSITION *cur_pos= &best_positions[tablenr];
    JOIN_TAB *tab= cur_pos->table;
    if (tab->table->is_splittable())
    {
      SplM_plan_info *spl_plan= cur_pos->spl_plan;
      if (tab->fix_splitting(spl_plan,
                             all_tables & ~prev_tables,
                             tablenr < const_tables ))
          return true;
    }
    prev_tables|= tab->table->map;
  }
  return false;
}


/**
  @brief
    Inject splitting conditions into WHERE of split derived

  @details
    The function calls JOIN_TAB::inject_best_splitting_cond() for each
    materialized derived table T used in this join for which the split
    optimization has been chosen by the optimizer. It is done in order to
    inject equalities pushed into the where clause of the specification
    of T that would be helpful to employ the splitting technique.

  @retval
    false  on success
    true   on failure
*/

bool JOIN::inject_splitting_cond_for_all_tables_with_split_opt()
{
  table_map prev_tables= 0;
  table_map all_tables= (table_map(1) << table_count) - 1;
  for (uint tablenr= 0; tablenr < table_count; tablenr++)
  {
    POSITION *cur_pos= &best_positions[tablenr];
    JOIN_TAB *tab= cur_pos->table;
    prev_tables|= tab->table->map;
    if (!(tab->table->is_splittable() && cur_pos->spl_plan))
      continue;
    SplM_opt_info *spl_opt_info= tab->table->spl_opt_info;
    JOIN *join= spl_opt_info->join;
    /*
      Currently the equalities referencing columns of SJM tables with
      look-up access cannot be pushed into materialized derived.
    */
    if (join->inject_best_splitting_cond((all_tables & ~prev_tables) |
				          sjm_lookup_tables))
        return true;
  }
  return false;
}
