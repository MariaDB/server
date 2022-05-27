#ifndef SQL_SELECT_INCLUDED
#define SQL_SELECT_INCLUDED

/* Copyright (c) 2000, 2013, Oracle and/or its affiliates.
   Copyright (c) 2008, 2022, MariaDB Corporation.

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

/**
  @file

  @brief
  classes to use when handling where clause
*/

#ifdef USE_PRAGMA_INTERFACE
#pragma interface			/* gcc class implementation */
#endif

#include "procedure.h"
#include "sql_array.h"                        /* Array */
#include "records.h"                          /* READ_RECORD */
#include "opt_range.h"                /* SQL_SELECT, QUICK_SELECT_I */
#include "filesort.h"

typedef struct st_join_table JOIN_TAB;
/* Values in optimize */
#define KEY_OPTIMIZE_EXISTS		1U
#define KEY_OPTIMIZE_REF_OR_NULL	2U
#define KEY_OPTIMIZE_EQ	                4U

inline uint get_hash_join_key_no() { return MAX_KEY; }

inline bool is_hash_join_key_no(uint key) { return key == MAX_KEY; }

typedef struct keyuse_t {
  TABLE *table;
  Item	*val;				/**< or value if no field */
  table_map used_tables;
  uint	key, keypart, optimize;
  key_part_map keypart_map;
  ha_rows      ref_table_rows;
  /**
    If true, the comparison this value was created from will not be
    satisfied if val has NULL 'value'.
  */
  bool null_rejecting;
  /*
    !NULL - This KEYUSE was created from an equality that was wrapped into
            an Item_func_trig_cond. This means the equality (and validity of 
            this KEYUSE element) can be turned on and off. The on/off state 
            is indicted by the pointed value:
              *cond_guard == TRUE <=> equality condition is on
              *cond_guard == FALSE <=> equality condition is off

    NULL  - Otherwise (the source equality can't be turned off)
  */
  bool *cond_guard;
  /*
     0..64    <=> This was created from semi-join IN-equality # sj_pred_no.
     MAX_UINT  Otherwise
  */
  uint         sj_pred_no;

  /*
    If this is NULL than KEYUSE is always enabled.
    Otherwise it points to the enabling flag for this keyuse (true <=> enabled)
  */
  bool *validity_ref;

  bool is_for_hash_join() { return is_hash_join_key_no(key); }
} KEYUSE;


struct KEYUSE_EXT: public KEYUSE
{
  /*
    This keyuse can be used only when the partial join being extended
    contains the tables from this table map
  */
  table_map needed_in_prefix;
  /* The enabling flag for keyuses usable for splitting */
  bool validity_var;
};

/// Used when finding key fields
struct KEY_FIELD {
  Field		*field;
  Item_bool_func *cond;
  Item		*val;			///< May be empty if diff constant
  uint		level;
  uint		optimize;
  bool		eq_func;
  /**
    If true, the condition this struct represents will not be satisfied
    when val IS NULL.
  */
  bool          null_rejecting;
  bool         *cond_guard; /* See KEYUSE::cond_guard */
  uint          sj_pred_no; /* See KEYUSE::sj_pred_no */
};


#define NO_KEYPART ((uint)(-1))

class store_key;

const int NO_REF_PART= uint(-1);

typedef struct st_table_ref
{
  bool		key_err;
  /** True if something was read into buffer in join_read_key.  */
  bool          has_record;
  uint          key_parts;                ///< num of ...
  uint          key_length;               ///< length of key_buff
  int           key;                      ///< key no
  uchar         *key_buff;                ///< value to look for with key
  uchar         *key_buff2;               ///< key_buff+key_length
  store_key     **key_copy;               //

  /*
    Bitmap of key parts which refer to constants. key_copy only has copiers for
    non-const key parts.
  */
  key_part_map  const_ref_part_map;

  Item          **items;                  ///< val()'s for each keypart
  /*  
    Array of pointers to trigger variables. Some/all of the pointers may be
    NULL.  The ref access can be used iff
    
      for each used key part i, (!cond_guards[i] || *cond_guards[i]) 

    This array is used by subquery code. The subquery code may inject
    triggered conditions, i.e. conditions that can be 'switched off'. A ref 
    access created from such condition is not valid when at least one of the 
    underlying conditions is switched off (see subquery code for more details)
  */
  bool          **cond_guards;
  /**
    (null_rejecting & (1<<i)) means the condition is '=' and no matching
    rows will be produced if items[i] IS NULL (see add_not_null_conds())
  */
  key_part_map  null_rejecting;
  table_map	depend_map;		  ///< Table depends on these tables.

  /* null byte position in the key_buf. Used for REF_OR_NULL optimization */
  uchar          *null_ref_key;
  /* 
    ref_or_null optimization: number of key part that alternates between
    the lookup value or NULL (there's only one such part). 
    If we're not using ref_or_null, the value is NO_REF_PART
  */
  uint           null_ref_part;

  /*
    The number of times the record associated with this key was used
    in the join.
  */
  ha_rows       use_count;

  /*
    TRUE <=> disable the "cache" as doing lookup with the same key value may
    produce different results (because of Index Condition Pushdown)

  */
  bool          disable_cache;

  /*
    If true, this ref access was constructed from equalities generated by
    LATERAL DERIVED (aka GROUP BY splitting) optimization
  */
  bool          uses_splitting;

  bool tmp_table_index_lookup_init(THD *thd, KEY *tmp_key, Item_iterator &it,
                                   bool value, uint skip= 0);
  bool is_access_triggered();
} TABLE_REF;


/*
  The structs which holds the join connections and join states
*/
enum join_type { JT_UNKNOWN,JT_SYSTEM,JT_CONST,JT_EQ_REF,JT_REF,JT_MAYBE_REF,
		 JT_ALL, JT_RANGE, JT_NEXT, JT_FT, JT_REF_OR_NULL,
		 JT_UNIQUE_SUBQUERY, JT_INDEX_SUBQUERY, JT_INDEX_MERGE,
                 JT_HASH, JT_HASH_RANGE, JT_HASH_NEXT, JT_HASH_INDEX_MERGE};

class JOIN;

enum enum_nested_loop_state
{
  NESTED_LOOP_KILLED= -2, NESTED_LOOP_ERROR= -1,
  NESTED_LOOP_OK= 0, NESTED_LOOP_NO_MORE_ROWS= 1,
  NESTED_LOOP_QUERY_LIMIT= 3, NESTED_LOOP_CURSOR_LIMIT= 4
};


/* Possible sj_strategy values */
enum sj_strategy_enum
{
  SJ_OPT_NONE=0,
  SJ_OPT_DUPS_WEEDOUT=1,
  SJ_OPT_LOOSE_SCAN  =2,
  SJ_OPT_FIRST_MATCH =3,
  SJ_OPT_MATERIALIZE =4,
  SJ_OPT_MATERIALIZE_SCAN=5
};

/* Values for JOIN_TAB::packed_info */
#define TAB_INFO_HAVE_VALUE 1U
#define TAB_INFO_USING_INDEX 2U
#define TAB_INFO_USING_WHERE 4U
#define TAB_INFO_FULL_SCAN_ON_NULL 8U

typedef enum_nested_loop_state
(*Next_select_func)(JOIN *, struct st_join_table *, bool);
Next_select_func setup_end_select_func(JOIN *join, JOIN_TAB *tab);
int rr_sequential(READ_RECORD *info);
int read_record_func_for_rr_and_unpack(READ_RECORD *info);
Item *remove_pushed_top_conjuncts(THD *thd, Item *cond);
Item *and_new_conditions_to_optimized_cond(THD *thd, Item *cond,
                                           COND_EQUAL **cond_eq,
                                           List<Item> &new_conds,
                                           Item::cond_result *cond_value);

#include "sql_explain.h"

/**************************************************************************************
 * New EXPLAIN structures END
 *************************************************************************************/

class JOIN_CACHE;
class SJ_TMP_TABLE;
class JOIN_TAB_RANGE;
class AGGR_OP;
class Filesort;
struct SplM_plan_info;
class SplM_opt_info;

typedef struct st_join_table {
  TABLE		*table;
  TABLE_LIST    *tab_list;
  KEYUSE	*keyuse;       /**< pointer to first used key */
  KEY           *hj_key;       /**< descriptor of the used best hash join key
                                    not supported by any index               */
  SQL_SELECT	*select;
  COND		*select_cond;
  COND          *on_precond;    /**< part of on condition to check before
                                     accessing the first inner table         */
  QUICK_SELECT_I *quick;
  /* 
    The value of select_cond before we've attempted to do Index Condition
    Pushdown. We may need to restore everything back if we first choose one
    index but then reconsider (see test_if_skip_sort_order() for such
    scenarios).
    NULL means no index condition pushdown was performed.
  */
  Item          *pre_idx_push_select_cond;
  /*
    Pointer to the associated ON expression. on_expr_ref=!NULL except for
    degenerate joins. 

    Optimization phase: *on_expr_ref!=NULL for tables that are the single
      tables on the inner side of the outer join (t1 LEFT JOIN t2 ON...)

    Execution phase: *on_expr_ref!=NULL for tables that are first inner tables
      within an outer join (which may have multiple tables)
  */
  Item	       **on_expr_ref;
  COND_EQUAL    *cond_equal;    /**< multiple equalities for the on expression */
  st_join_table *first_inner;   /**< first inner table for including outerjoin */
  bool           found;         /**< true after all matches or null complement */
  bool           not_null_compl;/**< true before null complement is added      */
  st_join_table *last_inner;    /**< last table table for embedding outer join */
  st_join_table *first_upper;  /**< first inner table for embedding outer join */
  st_join_table *first_unmatched; /**< used for optimization purposes only     */

  /*
    For join tabs that are inside an SJM bush: root of the bush
  */
  st_join_table *bush_root_tab;

  /* TRUE <=> This join_tab is inside an SJM bush and is the last leaf tab here */
  bool          last_leaf_in_bush;
  
  /*
    ptr  - this is a bush, and ptr points to description of child join_tab
           range
    NULL - this join tab has no bush children
  */
  JOIN_TAB_RANGE *bush_children;
  
  /* Special content for EXPLAIN 'Extra' column or NULL if none */
  enum explain_extra_tag info;
  
  Table_access_tracker *tracker;

  Table_access_tracker *jbuf_tracker;
  /* 
    Bitmap of TAB_INFO_* bits that encodes special line for EXPLAIN 'Extra'
    column, or 0 if there is no info.
  */
  uint          packed_info;

  //  READ_RECORD::Setup_func materialize_table;
  READ_RECORD::Setup_func read_first_record;
  Next_select_func next_select;
  READ_RECORD	read_record;
  /* 
    Currently the following two fields are used only for a [NOT] IN subquery
    if it is executed by an alternative full table scan when the left operand of
    the subquery predicate is evaluated to NULL.
  */  
  READ_RECORD::Setup_func save_read_first_record;/* to save read_first_record */
  READ_RECORD::Read_func save_read_record;/* to save read_record.read_record */
  double	worst_seeks;
  key_map	const_keys;			/**< Keys with constant part */
  key_map	checked_keys;			/**< Keys checked in find_best */
  key_map	needed_reg;
  key_map       keys;                           /**< all keys with can be used */

  /* Either #rows in the table or 1 for const table.  */
  ha_rows	records;
  /*
    Number of records that will be scanned (yes scanned, not returned) by the
    best 'independent' access method, i.e. table scan or QUICK_*_SELECT)
  */
  ha_rows       found_records;
  /*
    Cost of accessing the table using "ALL" or range/index_merge access
    method (but not 'index' for some reason), i.e. this matches method which
    E(#records) is in found_records.
  */
  double        read_time;
  
  /* Copy of POSITION::records_read, set by get_best_combination() */
  double        records_read;
  
  /* The selectivity of the conditions that can be pushed to the table */ 
  double        cond_selectivity;  
  
  /* Startup cost for execution */
  double        startup_cost;
    
  double        partial_join_cardinality;

  table_map	dependent,key_dependent;
  /*
     1 - use quick select
     2 - use "Range checked for each record"
  */
  uint		use_quick;
  /*
    Index to use. Note: this is valid only for 'index' access, but not range or
    ref access.
  */
  uint          index;
  uint		status;				///< Save status for cache
  uint		used_fields;
  ulong         used_fieldlength;
  ulong         max_used_fieldlength;
  uint          used_blobs;
  uint          used_null_fields;
  uint          used_uneven_bit_fields;
  enum join_type type;
  bool          cached_eq_ref_table,eq_ref_table;
  bool          shortcut_for_distinct;
  bool          sorted;
  /* 
    If it's not 0 the number stored this field indicates that the index
    scan has been chosen to access the table data and we expect to scan 
    this number of rows for the table.
  */ 
  ha_rows       limit; 
  TABLE_REF	ref;
  /* TRUE <=> condition pushdown supports other tables presence */
  bool          icp_other_tables_ok;
  /* 
    TRUE <=> condition pushed to the index has to be factored out of
    the condition pushed to the table
  */
  bool          idx_cond_fact_out;
  bool          use_join_cache;
  uint          used_join_cache_level;
  ulong         join_buffer_size_limit;
  JOIN_CACHE	*cache;
  /*
    Index condition for BKA access join
  */
  Item          *cache_idx_cond;
  SQL_SELECT    *cache_select;
  AGGR_OP       *aggr;
  JOIN		*join;
  /*
    Embedding SJ-nest (may be not the direct parent), or NULL if none.
    This variable holds the result of table pullout.
  */
  TABLE_LIST    *emb_sj_nest;

  /* FirstMatch variables (final QEP) */
  struct st_join_table *first_sj_inner_tab;
  struct st_join_table *last_sj_inner_tab;

  /* Variables for semi-join duplicate elimination */
  SJ_TMP_TABLE  *flush_weedout_table;
  SJ_TMP_TABLE  *check_weed_out_table;
  /* for EXPLAIN only: */
  SJ_TMP_TABLE  *first_weedout_table;

  /**
    reference to saved plan and execution statistics
  */
  Explain_table_access *explain_plan;

  /*
    If set, means we should stop join enumeration after we've got the first
    match and return to the specified join tab. May point to
    join->join_tab[-1] which means stop join execution after the first
    match.
  */
  struct st_join_table  *do_firstmatch;
 
  /* 
     ptr  - We're doing a LooseScan, this join tab is the first (i.e. 
            "driving") join tab), and ptr points to the last join tab
            handled by the strategy. loosescan_match_tab->found_match
            should be checked to see if the current value group had a match.
     NULL - Not doing a loose scan on this join tab.
  */
  struct st_join_table *loosescan_match_tab;
  
  /* TRUE <=> we are inside LooseScan range */
  bool inside_loosescan_range;

  /* Buffer to save index tuple to be able to skip duplicates */
  uchar *loosescan_buf;
  
  /* 
    Index used by LooseScan (we store it here separately because ref access
    stores it in tab->ref.key, while range scan stores it in tab->index, etc)
  */
  uint loosescan_key;

  /* Length of key tuple (depends on #keyparts used) to store in the above */
  uint loosescan_key_len;

  /* Used by LooseScan. TRUE<=> there has been a matching record combination */
  bool found_match;
  
  /*
    Used by DuplicateElimination. tab->table->ref must have the rowid
    whenever we have a current record.
  */
  int  keep_current_rowid;

  /* NestedOuterJoins: Bitmap of nested joins this table is part of */
  nested_join_map embedding_map;

  /* Tmp table info */
  TMP_TABLE_PARAM *tmp_table_param;

  /* Sorting related info */
  Filesort *filesort;
  SORT_INFO *filesort_result;
  
  /*
    Non-NULL value means this join_tab must do window function computation
    before reading.
  */
  Window_funcs_computation* window_funcs_step;

  /**
    List of topmost expressions in the select list. The *next* JOIN_TAB
    in the plan should use it to obtain correct values. Same applicable to
    all_fields. These lists are needed because after tmp tables functions
    will be turned to fields. These variables are pointing to
    tmp_fields_list[123]. Valid only for tmp tables and the last non-tmp
    table in the query plan.
    @see JOIN::make_aggr_tables_info()
  */
  List<Item> *fields;
  /** List of all expressions in the select list */
  List<Item> *all_fields;
  /*
    Pointer to the ref array slice which to switch to before sending
    records. Valid only for tmp tables.
  */
  Ref_ptr_array *ref_array;

  /** Number of records saved in tmp table */
  ha_rows send_records;

  /** HAVING condition for checking prior saving a record into tmp table*/
  Item *having;

  /** TRUE <=> remove duplicates on this table. */
  bool distinct;

  /*
    Semi-join strategy to be used for this join table. This is a copy of
    POSITION::sj_strategy field. This field is set up by the
    fix_semijoin_strategies_for_picked_join_order.
  */
  enum sj_strategy_enum sj_strategy;

  uint n_sj_tables;

  bool preread_init_done;

  /*
    Cost info to the range filter used when joining this join table
    (Defined when the best join order has been already chosen)
  */
  Range_rowid_filter_cost_info *range_rowid_filter_info;
  /* Rowid filter to be used when joining this join table */
  Rowid_filter *rowid_filter;
  /* Becomes true just after the used range filter has been built / filled */
  bool is_rowid_filter_built;

  void build_range_rowid_filter_if_needed();

  void cleanup();
  inline bool is_using_loose_index_scan()
  {
    const SQL_SELECT *sel= filesort ? filesort->select : select;
    return (sel && sel->quick &&
            (sel->quick->get_type() == QUICK_SELECT_I::QS_TYPE_GROUP_MIN_MAX));
  }
  bool is_using_agg_loose_index_scan ()
  {
    return (is_using_loose_index_scan() &&
            ((QUICK_GROUP_MIN_MAX_SELECT *)select->quick)->is_agg_distinct());
  }
  bool is_inner_table_of_semi_join_with_first_match()
  {
    return first_sj_inner_tab != NULL;
  }
  bool is_inner_table_of_semijoin()
  {
    return emb_sj_nest != NULL;
  }
  bool is_inner_table_of_outer_join()
  {
    return first_inner != NULL;
  }
  bool is_single_inner_of_semi_join_with_first_match()
  {
    return first_sj_inner_tab == this && last_sj_inner_tab == this;            
  }
  bool is_single_inner_of_outer_join()
  {
    return first_inner == this && first_inner->last_inner == this;
  }
  bool is_first_inner_for_outer_join()
  {
    return first_inner == this;
  }
  bool use_match_flag()
  {
    return is_first_inner_for_outer_join() || first_sj_inner_tab == this ; 
  }
  bool check_only_first_match()
  {
    return is_inner_table_of_semi_join_with_first_match() ||
           (is_inner_table_of_outer_join() &&
            table->reginfo.not_exists_optimize);
  }
  bool is_last_inner_table()
  {
    return (first_inner && first_inner->last_inner == this) ||
           last_sj_inner_tab == this;
  }
  /*
    Check whether the table belongs to a nest of inner tables of an
    outer join or to a nest of inner tables of a semi-join
  */
  bool is_nested_inner()
  {
    if (first_inner && 
        (first_inner != first_inner->last_inner || first_inner->first_upper))
      return TRUE;
    if (first_sj_inner_tab && first_sj_inner_tab != last_sj_inner_tab)
      return TRUE;
    return FALSE;
  }
  struct st_join_table *get_first_inner_table()
  {
    if (first_inner)
      return first_inner;
    return first_sj_inner_tab; 
  }
  void set_select_cond(COND *to, uint line)
  {
    DBUG_PRINT("info", ("select_cond changes %p -> %p at line %u tab %p",
                        select_cond, to, line, this));
    select_cond= to;
  }
  COND *set_cond(COND *new_cond)
  {
    COND *tmp_select_cond= select_cond;
    set_select_cond(new_cond, __LINE__);
    if (select)
      select->cond= new_cond;
    return tmp_select_cond;
  }
  void calc_used_field_length(bool max_fl);
  ulong get_used_fieldlength()
  {
    if (!used_fieldlength)
      calc_used_field_length(FALSE);
    return used_fieldlength;
  }
  ulong get_max_used_fieldlength()
  {
    if (!max_used_fieldlength)
      calc_used_field_length(TRUE);
    return max_used_fieldlength;
  }
  double get_partial_join_cardinality() { return partial_join_cardinality; }
  bool hash_join_is_possible();
  int make_scan_filter();
  bool is_ref_for_hash_join() { return is_hash_join_key_no(ref.key); }
  KEY *get_keyinfo_by_key_no(uint key) 
  {
    return (is_hash_join_key_no(key) ? hj_key : table->key_info+key);
  }
  double scan_time();
  ha_rows get_examined_rows();
  bool preread_init();

  bool pfs_batch_update(JOIN *join);

  bool is_sjm_nest() { return MY_TEST(bush_children); }
  
  /*
    If this join_tab reads a non-merged semi-join (also called jtbm), return
    the select's number.  Otherwise, return 0.
  */
  int get_non_merged_semijoin_select() const
  {
    Item_in_subselect *subq;
    if (table->pos_in_table_list && 
        (subq= table->pos_in_table_list->jtbm_subselect))
    {
      return subq->unit->first_select()->select_number;
    }
    return 0; /* Not a merged semi-join */
  }

  bool access_from_tables_is_allowed(table_map used_tables,
                                     table_map sjm_lookup_tables)
  {
    table_map used_sjm_lookup_tables= used_tables & sjm_lookup_tables;
    return !used_sjm_lookup_tables ||
           (emb_sj_nest && 
            !(used_sjm_lookup_tables & ~emb_sj_nest->sj_inner_tables));
  }

  bool keyuse_is_valid_for_access_in_chosen_plan(JOIN *join, KEYUSE *keyuse);

  void remove_redundant_bnl_scan_conds();

  bool save_explain_data(Explain_table_access *eta, table_map prefix_tables,
                         bool distinct, struct st_join_table *first_top_tab);

  bool use_order() const; ///< Use ordering provided by chosen index?
  bool sort_table();
  bool remove_duplicates();

  void partial_cleanup();
  void add_keyuses_for_splitting();
  SplM_plan_info *choose_best_splitting(double record_count,
                                        table_map remaining_tables);
  bool fix_splitting(SplM_plan_info *spl_plan, table_map remaining_tables,
                     bool is_const_table);
} JOIN_TAB;


#include "sql_join_cache.h"

enum_nested_loop_state
sub_select_cache(JOIN *join, JOIN_TAB *join_tab, bool end_of_records);
enum_nested_loop_state 
sub_select(JOIN *join, JOIN_TAB *join_tab, bool end_of_records);
enum_nested_loop_state
sub_select_postjoin_aggr(JOIN *join, JOIN_TAB *join_tab, bool end_of_records);

enum_nested_loop_state
end_send_group(JOIN *join, JOIN_TAB *join_tab __attribute__((unused)),
	       bool end_of_records);
enum_nested_loop_state
end_write_group(JOIN *join, JOIN_TAB *join_tab __attribute__((unused)),
		bool end_of_records);


class Semi_join_strategy_picker
{
public:
  /* Called when starting to build a new join prefix */
  virtual void set_empty() = 0;

  /* 
    Update internal state after another table has been added to the join
    prefix
  */
  virtual void set_from_prev(POSITION *prev) = 0;
  
  virtual bool check_qep(JOIN *join,
                         uint idx,
                         table_map remaining_tables, 
                         const JOIN_TAB *new_join_tab,
                         double *record_count,
                         double *read_time,
                         table_map *handled_fanout,
                         sj_strategy_enum *strategy,
                         POSITION *loose_scan_pos) = 0;

  virtual void mark_used() = 0;

  virtual ~Semi_join_strategy_picker() {} 
};


/*
  Duplicate Weedout strategy optimization state
*/

class Duplicate_weedout_picker : public Semi_join_strategy_picker
{
  /* The first table that the strategy will need to handle */
  uint  first_dupsweedout_table;

  /*
    Tables that we will need to have in the prefix to do the weedout step
    (all inner and all outer that the involved semi-joins are correlated with)
  */
  table_map dupsweedout_tables;
  
  bool is_used;
public:
  void set_empty()
  {
    dupsweedout_tables= 0;
    first_dupsweedout_table= MAX_TABLES;
    is_used= FALSE;
  }
  void set_from_prev(POSITION *prev);
  
  bool check_qep(JOIN *join,
                 uint idx,
                 table_map remaining_tables, 
                 const JOIN_TAB *new_join_tab,
                 double *record_count,
                 double *read_time,
                 table_map *handled_fanout,
                 sj_strategy_enum *stratey,
                 POSITION *loose_scan_pos);

  void mark_used() { is_used= TRUE; }
  friend void fix_semijoin_strategies_for_picked_join_order(JOIN *join);
};


class Firstmatch_picker : public Semi_join_strategy_picker
{
  /*
    Index of the first inner table that we intend to handle with this
    strategy
  */
  uint first_firstmatch_table;
  /*
    Tables that were not in the join prefix when we've started considering 
    FirstMatch strategy.
  */
  table_map first_firstmatch_rtbl;
  /* 
    Tables that need to be in the prefix before we can calculate the cost
    of using FirstMatch strategy.
   */
  table_map firstmatch_need_tables;

  bool is_used;

  bool in_firstmatch_prefix() { return (first_firstmatch_table != MAX_TABLES); }
  void invalidate_firstmatch_prefix() { first_firstmatch_table= MAX_TABLES; }
public:
  void set_empty()
  {
    invalidate_firstmatch_prefix();
    is_used= FALSE;
  }

  void set_from_prev(POSITION *prev);
  bool check_qep(JOIN *join,
                 uint idx,
                 table_map remaining_tables, 
                 const JOIN_TAB *new_join_tab,
                 double *record_count,
                 double *read_time,
                 table_map *handled_fanout,
                 sj_strategy_enum *strategy,
                 POSITION *loose_scan_pos);

  void mark_used() { is_used= TRUE; }
  friend void fix_semijoin_strategies_for_picked_join_order(JOIN *join);
};


class LooseScan_picker : public Semi_join_strategy_picker
{
public:
  /* The first (i.e. driving) table we're doing loose scan for */
  uint        first_loosescan_table;
  /* 
     Tables that need to be in the prefix before we can calculate the cost
     of using LooseScan strategy.
  */
  table_map   loosescan_need_tables;

  /*
    keyno  -  Planning to do LooseScan on this key. If keyuse is NULL then 
              this is a full index scan, otherwise this is a ref+loosescan
              scan (and keyno matches the KEUSE's)
    MAX_KEY - Not doing a LooseScan
  */
  uint loosescan_key;  // final (one for strategy instance )
  uint loosescan_parts; /* Number of keyparts to be kept distinct */
  
  bool is_used;
  void set_empty()
  {
    first_loosescan_table= MAX_TABLES; 
    is_used= FALSE;
  }

  void set_from_prev(POSITION *prev);
  bool check_qep(JOIN *join,
                 uint idx,
                 table_map remaining_tables, 
                 const JOIN_TAB *new_join_tab,
                 double *record_count,
                 double *read_time,
                 table_map *handled_fanout,
                 sj_strategy_enum *strategy,
                 POSITION *loose_scan_pos);
  void mark_used() { is_used= TRUE; }

  friend class Loose_scan_opt;
  friend void best_access_path(JOIN      *join,
                               JOIN_TAB  *s,
                               table_map remaining_tables,
                               const POSITION *join_positions,
                               uint      idx,
                               bool      disable_jbuf,
                               double    record_count,
                               POSITION *pos,
                               POSITION *loose_scan_pos);
  friend bool get_best_combination(JOIN *join);
  friend int setup_semijoin_loosescan(JOIN *join);
  friend void fix_semijoin_strategies_for_picked_join_order(JOIN *join);
};


class Sj_materialization_picker : public Semi_join_strategy_picker
{
  bool is_used;

  /* The last inner table (valid once we're after it) */
  uint      sjm_scan_last_inner;
  /*
    Tables that we need to have in the prefix to calculate the correct cost.
    Basically, we need all inner tables and outer tables mentioned in the
    semi-join's ON expression so we can correctly account for fanout.
  */
  table_map sjm_scan_need_tables;

public:
  void set_empty()
  {
    sjm_scan_need_tables= 0;
    sjm_scan_last_inner= 0;
    is_used= FALSE;
  }
  void set_from_prev(POSITION *prev);
  bool check_qep(JOIN *join,
                 uint idx,
                 table_map remaining_tables, 
                 const JOIN_TAB *new_join_tab,
                 double *record_count,
                 double *read_time,
                 table_map *handled_fanout,
                 sj_strategy_enum *strategy,
                 POSITION *loose_scan_pos);
  void mark_used() { is_used= TRUE; }

  friend void fix_semijoin_strategies_for_picked_join_order(JOIN *join);
};


class Range_rowid_filter_cost_info;
class Rowid_filter;


/**
  Information about a position of table within a join order. Used in join
  optimization.
*/
class POSITION
{
public:
  /* The table that's put into join order */
  JOIN_TAB *table;

  /*
    The "fanout": number of output rows that will be produced (after
    pushed down selection condition is applied) per each row combination of
    previous tables.
  */
  double records_read;

  /* The selectivity of the pushed down conditions */
  double cond_selectivity;

  /* 
    Cost accessing the table in course of the entire complete join execution,
    i.e. cost of one access method use (e.g. 'range' or 'ref' scan ) times 
    number the access method will be invoked.
  */
  double read_time;

  double    prefix_record_count;

  /*
    NULL  -  'index' or 'range' or 'index_merge' or 'ALL' access is used.
    Other - [eq_]ref[_or_null] access is used. Pointer to {t.keypart1 = expr}
  */
  KEYUSE *key;

  /* Info on splitting plan used at this position */
  SplM_plan_info *spl_plan;

  /* Cost info for the range filter used at this position */
  Range_rowid_filter_cost_info *range_rowid_filter_info;

  /* If ref-based access is used: bitmap of tables this table depends on  */
  table_map ref_depend_map;

  /*
    Bitmap of semi-join inner tables that are in the join prefix and for
    which there's no provision for how to eliminate semi-join duplicates
    they produce.
  */
  table_map dups_producing_tables;

  table_map inner_tables_handled_with_other_sjs;

  Duplicate_weedout_picker  dups_weedout_picker;
  Firstmatch_picker         firstmatch_picker;
  LooseScan_picker          loosescan_picker;
  Sj_materialization_picker sjmat_picker;

  /* Cumulative cost and record count for the join prefix */
  Cost_estimate prefix_cost;

  /*
    Current optimization state: Semi-join strategy to be used for this
    and preceding join tables.

    Join optimizer sets this for the *last* join_tab in the
    duplicate-generating range. That is, in order to interpret this field,
    one needs to traverse join->[best_]positions array from right to left.
    When you see a join table with sj_strategy!= SJ_OPT_NONE, some other
    field (depending on the strategy) tells how many preceding positions
    this applies to. The values of covered_preceding_positions->sj_strategy
    must be ignored.
  */
  enum sj_strategy_enum sj_strategy;

  /* Type of join (EQ_REF, REF etc) */
  enum join_type type;
  /*
    Valid only after fix_semijoin_strategies_for_picked_join_order() call:
    if sj_strategy!=SJ_OPT_NONE, this is the number of subsequent tables that
    are covered by the specified semi-join strategy
  */
  uint n_sj_tables;

  /*
    TRUE <=> join buffering will be used. At the moment this is based on
    *very* imprecise guesses made in best_access_path().
  */
  bool use_join_buffer;
  POSITION();
};

typedef Bounds_checked_array<Item_null_result*> Item_null_array;

typedef struct st_rollup
{
  enum State { STATE_NONE, STATE_INITED, STATE_READY };
  State state;
  Item_null_array null_items;
  Ref_ptr_array *ref_pointer_arrays;
  List<Item> *fields;
} ROLLUP;


class JOIN_TAB_RANGE: public Sql_alloc
{
public:
  JOIN_TAB *start;
  JOIN_TAB *end;
};

class Pushdown_query;

/**
  @brief
    Class to perform postjoin aggregation operations

  @details
    The result records are obtained on the put_record() call.
    The aggrgation process is determined by the write_func, it could be:
      end_write          Simply store all records in tmp table.
      end_write_group    Perform grouping using join->group_fields,
                         records are expected to be sorted.
      end_update         Perform grouping using the key generated on tmp
                         table. Input records aren't expected to be sorted.
                         Tmp table uses the heap engine
      end_update_unique  Same as above, but the engine is myisam.

    Lazy table initialization is used - the table will be instantiated and
    rnd/index scan started on the first put_record() call.

*/

class AGGR_OP :public Sql_alloc
{
public:
  JOIN_TAB *join_tab;

  AGGR_OP(JOIN_TAB *tab) : join_tab(tab), write_func(NULL)
  {};

  enum_nested_loop_state put_record() { return put_record(false); };
  /*
    Send the result of operation further (to a next operation/client)
    This function is called after all records were put into tmp table.

    @return return one of enum_nested_loop_state values.
  */
  enum_nested_loop_state end_send();
  /** write_func setter */
  void set_write_func(Next_select_func new_write_func)
  {
    write_func= new_write_func;
  }

private:
  /** Write function that would be used for saving records in tmp table. */
  Next_select_func write_func;
  enum_nested_loop_state put_record(bool end_of_records);
  bool prepare_tmp_table();
};


class JOIN :public Sql_alloc
{
private:
  JOIN(const JOIN &rhs);                        /**< not implemented */
  JOIN& operator=(const JOIN &rhs);             /**< not implemented */

protected:

  /**
    The subset of the state of a JOIN that represents an optimized query
    execution plan. Allows saving/restoring different JOIN plans for the same
    query.
  */
  class Join_plan_state {
  public:
    DYNAMIC_ARRAY keyuse;        /* Copy of the JOIN::keyuse array. */
    POSITION *best_positions;    /* Copy of JOIN::best_positions */
    /* Copies of the JOIN_TAB::keyuse pointers for each JOIN_TAB. */
    KEYUSE **join_tab_keyuse;
    /* Copies of JOIN_TAB::checked_keys for each JOIN_TAB. */
    key_map *join_tab_checked_keys;
    SJ_MATERIALIZATION_INFO **sj_mat_info;
    my_bool error;
  public:
    Join_plan_state(uint tables) : error(0)
    {   
      keyuse.elements= 0;
      keyuse.buffer= NULL;
      keyuse.malloc_flags= 0;
      best_positions= 0;                        /* To detect errors */
      error= my_multi_malloc(PSI_INSTRUMENT_ME, MYF(MY_WME),
                             &best_positions,
                             sizeof(*best_positions) * (tables + 1),
                             &join_tab_keyuse,
                             sizeof(*join_tab_keyuse) * tables,
                             &join_tab_checked_keys,
                             sizeof(*join_tab_checked_keys) * tables,
                             &sj_mat_info,
                             sizeof(sj_mat_info) * tables,
                             NullS) == 0;
    }
    Join_plan_state(JOIN *join);
    ~Join_plan_state()
    {
      delete_dynamic(&keyuse);
      my_free(best_positions);
    }
  };

  /* Results of reoptimizing a JOIN via JOIN::reoptimize(). */
  enum enum_reopt_result {
    REOPT_NEW_PLAN, /* there is a new reoptimized plan */
    REOPT_OLD_PLAN, /* no new improved plan can be found, use the old one */
    REOPT_ERROR,    /* an irrecovarable error occurred during reoptimization */
    REOPT_NONE      /* not yet reoptimized */
  };

  /* Support for plan reoptimization with rewritten conditions. */
  enum_reopt_result reoptimize(Item *added_where, table_map join_tables,
                               Join_plan_state *save_to);
  /* Choose a subquery plan for a table-less subquery. */
  bool choose_tableless_subquery_plan();
  void handle_implicit_grouping_with_window_funcs();

public:
  void save_query_plan(Join_plan_state *save_to);
  void reset_query_plan();
  void restore_query_plan(Join_plan_state *restore_from);

public:
  JOIN_TAB *join_tab, **best_ref;

  /* List of fields that aren't under an aggregate function */
  List<Item_field> non_agg_fields;

  JOIN_TAB **map2table;    ///< mapping between table indexes and JOIN_TABs
  List<JOIN_TAB_RANGE> join_tab_ranges;
  
  /*
    Base tables participating in the join. After join optimization is done, the
    tables are stored in the join order (but the only really important part is 
    that const tables are first).
  */
  TABLE    **table;
  /**
    The table which has an index that allows to produce the requried ordering.
    A special value of 0x1 means that the ordering will be produced by
    passing 1st non-const table to filesort(). NULL means no such table exists.
  */
  TABLE    *sort_by_table;
  /* 
    Number of tables in the join. 
    (In MySQL, it is named 'tables' and is also the number of elements in 
     join->join_tab array. In MariaDB, the latter is not true, so we've renamed
     the variable)
  */
  uint	   table_count;
  uint     outer_tables;  /**< Number of tables that are not inside semijoin */
  uint     const_tables;
  /* 
    Number of tables in the top join_tab array. Normally this matches
    (join_tab_ranges.head()->end - join_tab_ranges.head()->start). 
    
    We keep it here so that it is saved/restored with JOIN::restore_tmp.
  */
  uint     top_join_tab_count;
  uint     aggr_tables;     ///< Number of post-join tmp tables 
  uint	   send_group_parts;
  /*
    This represents the number of items in ORDER BY *after* removing
    all const items. This is computed before other optimizations take place,
    such as removal of ORDER BY when it is a prefix of GROUP BY, for example:
    GROUP BY a, b ORDER BY a

    This is used when deciding to send rows, by examining the correct number
    of items in the group_fields list when ORDER BY was previously eliminated.
  */
  uint with_ties_order_count;
  /*
    True if the query has GROUP BY.
    (that is, if group_by != NULL. when DISTINCT is converted into GROUP BY, it
     will set this, too. It is not clear why we need a separate var from 
     group_list)
  */
  bool	   group;
  bool     need_distinct;

  /**
    Indicates that grouping will be performed on the result set during
    query execution. This field belongs to query execution.

    If 'sort_and_group' is set, then the optimizer is going to use on of
    the following algorithms to resolve GROUP BY.

    - If one table, sort the table and then calculate groups on the fly.
    - If more than one table, create a temporary table to hold the join,
      sort it and then resolve group by on the fly.

    The 'on the fly' calculation is done in end_send_group()

    @see make_group_fields, alloc_group_fields, JOIN::exec,
         setup_end_select_func
  */
  bool     sort_and_group; 
  bool     first_record,full_join, no_field_update;
  bool     hash_join;
  bool	   do_send_rows;
  table_map const_table_map;
  /** 
    Bitmap of semijoin tables that the current partial plan decided
    to materialize and access by lookups
  */
  table_map sjm_lookup_tables;
  /** 
    Bitmap of semijoin tables that the chosen plan decided
    to materialize to scan the results of materialization
  */
  table_map sjm_scan_tables;
  /*
    Constant tables for which we have found a row (as opposed to those for
    which we didn't).
  */
  table_map found_const_table_map;
  
  /* Tables removed by table elimination. Set to 0 before the elimination. */
  table_map eliminated_tables;
  /*
     Bitmap of all inner tables from outer joins (set at start of
     make_join_statistics)
  */
  table_map outer_join;
  /* Bitmap of tables used in the select list items */
  table_map select_list_used_tables;
  ha_rows  send_records,found_records,join_examined_rows, accepted_rows;

  /*
    LIMIT for the JOIN operation. When not using aggregation or DISITNCT, this 
    is the same as select's LIMIT clause specifies.
    Note that this doesn't take sql_calc_found_rows into account.
  */
  ha_rows row_limit;

  /*
    How many output rows should be produced after GROUP BY.
    (if sql_calc_found_rows is used, LIMIT is ignored)
  */
  ha_rows select_limit;
  /*
    Number of duplicate rows found in UNION.
  */
  ha_rows duplicate_rows;
  /**
    Used to fetch no more than given amount of rows per one
    fetch operation of server side cursor.
    The value is checked in end_send and end_send_group in fashion, similar
    to offset_limit_cnt:
      - fetch_limit= HA_POS_ERROR if there is no cursor.
      - when we open a cursor, we set fetch_limit to 0,
      - on each fetch iteration we add num_rows to fetch to fetch_limit
    NOTE: currently always HA_POS_ERROR.
  */
  ha_rows  fetch_limit;

  /* Finally picked QEP. This is result of join optimization */
  POSITION *best_positions;

  Pushdown_query *pushdown_query;
  JOIN_TAB *original_join_tab;
  uint	   original_table_count;

/******* Join optimization state members start *******/
  /*
    pointer - we're doing optimization for a semi-join materialization nest.
    NULL    - otherwise
  */
  TABLE_LIST *emb_sjm_nest;
  
  /* Current join optimization state */
  POSITION *positions;
  
  /*
    Bitmap of nested joins embedding the position at the end of the current 
    partial join (valid only during join optimizer run).
  */
  nested_join_map cur_embedding_map;
  
  /*
    Bitmap of inner tables of semi-join nests that have a proper subset of
    their tables in the current join prefix. That is, of those semi-join
    nests that have their tables both in and outside of the join prefix.
  */
  table_map cur_sj_inner_tables;
  
  /* We also maintain a stack of join optimization states in * join->positions[] */
/******* Join optimization state members end *******/

  /*
    Tables within complex firstmatch ranges (i.e. those where inner tables are
    interleaved with outer tables). Join buffering cannot be used for these.
  */
  table_map complex_firstmatch_tables;

  Next_select_func first_select;
  /*
    The cost of best complete join plan found so far during optimization,
    after optimization phase - cost of picked join order (not taking into
    account the changes made by test_if_skip_sort_order()).
  */
  double   best_read;
  /*
    Estimated result rows (fanout) of the join operation. If this is a subquery
    that is reexecuted multiple times, this value includes the estiamted # of
    reexecutions. This value is equal to the multiplication of all
    join->positions[i].records_read of a JOIN.
  */
  double   join_record_count;
  List<Item> *fields;

  /* Used only for FETCH ... WITH TIES to identify peers. */
  List<Cached_item> order_fields;
  /* Used during GROUP BY operations to identify when a group has changed. */
  List<Cached_item> group_fields, group_fields_cache;
  THD	   *thd;
  Item_sum  **sum_funcs, ***sum_funcs_end;
  /** second copy of sumfuncs (for queries with 2 temporary tables */
  Item_sum  **sum_funcs2, ***sum_funcs_end2;
  Procedure *procedure;
  Item	    *having;
  Item      *tmp_having; ///< To store having when processed temporary table
  Item      *having_history; ///< Store having for explain
  ORDER     *group_list_for_estimates;
  bool      having_is_correlated;
  ulonglong  select_options;
  /* 
    Bitmap of allowed types of the join caches that
    can be used for join operations
  */
  uint allowed_join_cache_types;
  bool allowed_semijoin_with_cache;
  bool allowed_outer_join_with_cache;
  /* Maximum level of the join caches that can be used for join operations */ 
  uint max_allowed_join_cache_level;
  select_result *result;
  TMP_TABLE_PARAM tmp_table_param;
  MYSQL_LOCK *lock;
  /// unit structure (with global parameters) for this select
  SELECT_LEX_UNIT *unit;
  /// select that processed
  SELECT_LEX *select_lex;
  /** 
    TRUE <=> optimizer must not mark any table as a constant table.
    This is needed for subqueries in form "a IN (SELECT .. UNION SELECT ..):
    when we optimize the select that reads the results of the union from a
    temporary table, we must not mark the temp. table as constant because
    the number of rows in it may vary from one subquery execution to another.
  */
  bool no_const_tables; 
  /*
    This flag is set if we call no_rows_in_result() as par of end_group().
    This is used as a simple speed optimization to avoiding calling
    restore_no_rows_in_result() in ::reinit()
  */
  bool no_rows_in_result_called;

  /**
    This is set if SQL_CALC_ROWS was calculated by filesort()
    and should be taken from the appropriate JOIN_TAB
  */
  bool filesort_found_rows;

  bool subq_exit_fl;
  
  ROLLUP rollup;				///< Used with rollup
  
  bool mixed_implicit_grouping;
  bool select_distinct;				///< Set if SELECT DISTINCT
  /**
    If we have the GROUP BY statement in the query,
    but the group_list was emptied by optimizer, this
    flag is TRUE.
    It happens when fields in the GROUP BY are from
    constant table
  */
  bool group_optimized_away;

  /*
    simple_xxxxx is set if ORDER/GROUP BY doesn't include any references
    to other tables than the first non-constant table in the JOIN.
    It's also set if ORDER/GROUP BY is empty.
    Used for deciding for or against using a temporary table to compute 
    GROUP/ORDER BY.
  */
  bool simple_order, simple_group;
  /*
    Set to 1 if any field in field list has RAND_TABLE set. For example if
    if one uses RAND() or ROWNUM() in field list
  */
  bool rand_table_in_field_list;

  /*
    ordered_index_usage is set if an ordered index access
    should be used instead of a filesort when computing 
    ORDER/GROUP BY.
  */
  enum
  {
    ordered_index_void,       // No ordered index avail.
    ordered_index_group_by,   // Use index for GROUP BY
    ordered_index_order_by    // Use index for ORDER BY
  } ordered_index_usage;

  /**
    Is set only in case if we have a GROUP BY clause
    and no ORDER BY after constant elimination of 'order'.
  */
  bool no_order;
  /** Is set if we have a GROUP BY and we have ORDER BY on a constant. */
  bool          skip_sort_order;

  bool need_tmp; 
  bool hidden_group_fields;
  /* TRUE if there was full cleunap of the JOIN */
  bool cleaned;
  DYNAMIC_ARRAY keyuse;
  Item::cond_result cond_value, having_value;
  /**
    Impossible where after reading const tables 
    (set in make_join_statistics())
  */
  bool impossible_where; 
  List<Item> all_fields; ///< to store all fields that used in query
  ///Above list changed to use temporary table
  List<Item> tmp_all_fields1, tmp_all_fields2, tmp_all_fields3;
  ///Part, shared with list above, emulate following list
  List<Item> tmp_fields_list1, tmp_fields_list2, tmp_fields_list3;
  List<Item> &fields_list; ///< hold field list passed to mysql_select
  List<Item> procedure_fields_list;
  int error;

  ORDER *order, *group_list, *proc_param; //hold parameters of mysql_select
  COND *conds;                            // ---"---
  Item *conds_history;                    // store WHERE for explain
  COND *outer_ref_cond;       ///<part of conds containing only outer references
  COND *pseudo_bits_cond;     // part of conds containing special bita
  TABLE_LIST *tables_list;           ///<hold 'tables' parameter of mysql_select
  List<TABLE_LIST> *join_list;       ///< list of joined tables in reverse order
  COND_EQUAL *cond_equal;
  COND_EQUAL *having_equal;
  /*
    Constant codition computed during optimization, but evaluated during
    join execution. Typically expensive conditions that should not be
    evaluated at optimization time.
  */
  Item *exec_const_cond;
  /*
    Constant ORDER and/or GROUP expressions that contain subqueries. Such
    expressions need to evaluated to verify that the subquery indeed
    returns a single row. The evaluation of such expressions is delayed
    until query execution.
  */
  List<Item> exec_const_order_group_cond;
  SQL_SELECT *select;                ///<created in optimisation phase
  JOIN_TAB *return_tab;              ///<used only for outer joins

  /*
    Used pointer reference for this select.
    select_lex->ref_pointer_array contains five "slices" of the same length:
    |========|========|========|========|========|
     ref_ptrs items0   items1   items2   items3
   */
  Ref_ptr_array ref_ptrs;
  // Copy of the initial slice above, to be used with different lists
  Ref_ptr_array items0, items1, items2, items3;
  // Used by rollup, to restore ref_ptrs after overwriting it.
  Ref_ptr_array current_ref_ptrs;

  const char *zero_result_cause; ///< not 0 if exec must return zero result
  
  bool union_part; ///< this subselect is part of union 

  enum join_optimization_state { NOT_OPTIMIZED=0,
                                 OPTIMIZATION_IN_PROGRESS=1,
                                 OPTIMIZATION_PHASE_1_DONE=2,
                                 OPTIMIZATION_DONE=3};
  // state of JOIN optimization
  enum join_optimization_state optimization_state;
  bool initialized; ///< flag to avoid double init_execution calls

  Explain_select *explain;
  
  enum { QEP_NOT_PRESENT_YET, QEP_AVAILABLE, QEP_DELETED} have_query_plan;

  // if keep_current_rowid=true, whether they should be saved in temporary table
  bool tmp_table_keep_current_rowid;

  /*
    Additional WHERE and HAVING predicates to be considered for IN=>EXISTS
    subquery transformation of a JOIN object.
  */
  Item *in_to_exists_where;
  Item *in_to_exists_having;
  
  /* Temporary tables used to weed-out semi-join duplicates */
  List<TABLE> sj_tmp_tables;
  /* SJM nests that are executed with SJ-Materialization strategy */
  List<SJ_MATERIALIZATION_INFO> sjm_info_list;

  /** TRUE <=> ref_pointer_array is set to items3. */
  bool set_group_rpa;
  /** Exec time only: TRUE <=> current group has been sent */
  bool group_sent;
  /**
    TRUE if the query contains an aggregate function but has no GROUP
    BY clause. 
  */
  bool implicit_grouping; 

  bool with_two_phase_optimization;

  /* Saved execution plan for this join */
  Join_plan_state *save_qep;
  /* Info on splittability of the table materialized by this plan*/
  SplM_opt_info *spl_opt_info;
  /* Contains info on keyuses usable for splitting */
  Dynamic_array<KEYUSE_EXT> *ext_keyuses_for_splitting;

  JOIN_TAB *sort_and_group_aggr_tab;
  /*
    Flag is set to true if select_lex was found to be degenerated before
    the optimize_cond() call in JOIN::optimize_inner() method.
  */
  bool is_orig_degenerated;

  JOIN(THD *thd_arg, List<Item> &fields_arg, ulonglong select_options_arg,
       select_result *result_arg)
    :fields_list(fields_arg)
  {
    init(thd_arg, fields_arg, select_options_arg, result_arg);
  }

  void init(THD *thd_arg, List<Item> &fields_arg, ulonglong select_options_arg,
            select_result *result_arg);

  /* True if the plan guarantees that it will be returned zero or one row */
  bool only_const_tables()  { return const_tables == table_count; }
  /* Number of tables actually joined at the top level */
  uint exec_join_tab_cnt() { return tables_list ? top_join_tab_count : 0; }

  /*
    Number of tables in the join which also includes the temporary tables
    created for GROUP BY, DISTINCT , WINDOW FUNCTION etc.
  */
  uint total_join_tab_cnt()
  {
    return exec_join_tab_cnt() + aggr_tables - 1;
  }

  int prepare(TABLE_LIST *tables, COND *conds, uint og_num, ORDER *order,
              bool skip_order_by, ORDER *group, Item *having,
              ORDER *proc_param, SELECT_LEX *select, SELECT_LEX_UNIT *unit);
  bool prepare_stage2();
  int optimize();
  int optimize_inner();
  int optimize_stage2();
  bool build_explain();
  int reinit();
  int init_execution();
  void exec();

  void exec_inner();
  bool prepare_result(List<Item> **columns_list);
  int destroy();
  void restore_tmp();
  bool alloc_func_list();
  bool flatten_subqueries();
  bool optimize_unflattened_subqueries();
  bool optimize_constant_subqueries();
  bool make_range_rowid_filters();
  bool init_range_rowid_filters();
  bool make_sum_func_list(List<Item> &all_fields, List<Item> &send_fields,
			  bool before_group_by);

  /// Initialzes a slice, see comments for ref_ptrs above.
  Ref_ptr_array ref_ptr_array_slice(size_t slice_num)
  {
    size_t slice_sz= select_lex->ref_pointer_array.size() / 5U;
    DBUG_ASSERT(select_lex->ref_pointer_array.size() % 5 == 0);
    DBUG_ASSERT(slice_num < 5U);
    return Ref_ptr_array(&select_lex->ref_pointer_array[slice_num * slice_sz],
                         slice_sz);
  }

  /**
     Overwrites one slice with the contents of another slice.
     In the normal case, dst and src have the same size().
     However: the rollup slices may have smaller size than slice_sz.
   */
  void copy_ref_ptr_array(Ref_ptr_array dst_arr, Ref_ptr_array src_arr)
  {
    DBUG_ASSERT(dst_arr.size() >= src_arr.size());
    if (src_arr.size() == 0)
      return;

    void *dest= dst_arr.array();
    const void *src= src_arr.array();
    memcpy(dest, src, src_arr.size() * src_arr.element_size());
  }

  /// Overwrites 'ref_ptrs' and remembers the the source as 'current'.
  void set_items_ref_array(Ref_ptr_array src_arr)
  {
    copy_ref_ptr_array(ref_ptrs, src_arr);
    current_ref_ptrs= src_arr;
  }

  /// Initializes 'items0' and remembers that it is 'current'.
  void init_items_ref_array()
  {
    items0= ref_ptr_array_slice(1);
    copy_ref_ptr_array(items0, ref_ptrs);
    current_ref_ptrs= items0;
  }

  bool rollup_init();
  bool rollup_process_const_fields();
  bool rollup_make_fields(List<Item> &all_fields, List<Item> &fields,
			  Item_sum ***func);
  int rollup_send_data(uint idx);
  int rollup_write_data(uint idx, TMP_TABLE_PARAM *tmp_table_param, TABLE *table);
  void join_free();
  /** Cleanup this JOIN, possibly for reuse */
  void cleanup(bool full);
  void clear();
  bool send_row_on_empty_set()
  {
    return (do_send_rows && implicit_grouping && !group_optimized_away &&
            having_value != Item::COND_FALSE);
  }
  bool empty_result() { return (zero_result_cause && !implicit_grouping); }
  bool change_result(select_result *new_result, select_result *old_result);
  bool is_top_level_join() const
  {
    return (unit == &thd->lex->unit && (unit->fake_select_lex == 0 ||
                                        select_lex == unit->fake_select_lex));
  }
  void cache_const_exprs();
  inline table_map all_tables_map()
  {
    return (table_map(1) << table_count) - 1;
  }
  void drop_unused_derived_keys();
  bool get_best_combination();
  bool add_sorting_to_table(JOIN_TAB *tab, ORDER *order);
  inline void eval_select_list_used_tables();
  /* 
    Return the table for which an index scan can be used to satisfy 
    the sort order needed by the ORDER BY/(implicit) GROUP BY clause 
  */
  JOIN_TAB *get_sort_by_join_tab()
  {
    return (need_tmp || !sort_by_table || skip_sort_order ||
            ((group || tmp_table_param.sum_func_count) && !group_list)) ?
              NULL : join_tab+const_tables;
  }
  bool setup_subquery_caches();
  bool shrink_join_buffers(JOIN_TAB *jt, 
                           ulonglong curr_space,
                           ulonglong needed_space);
  void set_allowed_join_cache_types();
  bool is_allowed_hash_join_access()
  { 
    return MY_TEST(allowed_join_cache_types & JOIN_CACHE_HASHED_BIT) &&
           max_allowed_join_cache_level > JOIN_CACHE_HASHED_BIT;
  }
  /*
    Check if we need to create a temporary table.
    This has to be done if all tables are not already read (const tables)
    and one of the following conditions holds:
    - We are using DISTINCT (simple distinct's are already optimized away)
    - We are using an ORDER BY or GROUP BY on fields not in the first table
    - We are using different ORDER BY and GROUP BY orders
    - The user wants us to buffer the result.
    - We are using WINDOW functions.
    When the WITH ROLLUP modifier is present, we cannot skip temporary table
    creation for the DISTINCT clause just because there are only const tables.
  */
  bool test_if_need_tmp_table()
  {
    return ((const_tables != table_count &&
	    ((select_distinct || !simple_order || !simple_group) ||
	     (group_list && order) ||
             MY_TEST(select_options & OPTION_BUFFER_RESULT))) ||
            (rollup.state != ROLLUP::STATE_NONE && select_distinct) ||
            select_lex->have_window_funcs());
  }
  bool choose_subquery_plan(table_map join_tables);
  void get_partial_cost_and_fanout(int end_tab_idx,
                                   table_map filter_map,
                                   double *read_time_arg, 
                                   double *record_count_arg);
  void get_prefix_cost_and_fanout(uint n_tables, 
                                  double *read_time_arg,
                                  double *record_count_arg);
  double get_examined_rows();
  /* defined in opt_subselect.cc */
  bool transform_max_min_subquery();
  /* True if this JOIN is a subquery under an IN predicate. */
  bool is_in_subquery()
  {
    return (unit->item && unit->item->is_in_predicate());
  }
  bool save_explain_data(Explain_query *output, bool can_overwrite,
                         bool need_tmp_table, bool need_order, bool distinct);
  int save_explain_data_intern(Explain_query *output, bool need_tmp_table,
                               bool need_order, bool distinct,
                               const char *message);
  JOIN_TAB *first_breadth_first_tab() { return join_tab; }
  bool check_two_phase_optimization(THD *thd);
  bool inject_cond_into_where(Item *injected_cond);
  bool check_for_splittable_materialized();
  void add_keyuses_for_splitting();
  bool inject_best_splitting_cond(table_map remaining_tables);
  bool fix_all_splittings_in_plan();
  bool inject_splitting_cond_for_all_tables_with_split_opt();
  void make_notnull_conds_for_range_scans();

  bool transform_in_predicates_into_in_subq(THD *thd);

  bool optimize_upper_rownum_func();

private:
  /**
    Create a temporary table to be used for processing DISTINCT/ORDER
    BY/GROUP BY.

    @note Will modify JOIN object wrt sort/group attributes

    @param tab              the JOIN_TAB object to attach created table to
    @param tmp_table_fields List of items that will be used to define
                            column types of the table.
    @param tmp_table_group  Group key to use for temporary table, NULL if none.
    @param save_sum_fields  If true, do not replace Item_sum items in 
                            @c tmp_fields list with Item_field items referring 
                            to fields in temporary table.

    @returns false on success, true on failure
  */
  bool create_postjoin_aggr_table(JOIN_TAB *tab, List<Item> *tmp_table_fields,
                                  ORDER *tmp_table_group,
                                  bool save_sum_fields,
                                  bool distinct,
                                  bool keep_row_ordermake);
  /**
    Optimize distinct when used on a subset of the tables.

    E.g.,: SELECT DISTINCT t1.a FROM t1,t2 WHERE t1.b=t2.b
    In this case we can stop scanning t2 when we have found one t1.a
  */
  void optimize_distinct();

  void cleanup_item_list(List<Item> &items) const;
  bool add_having_as_table_cond(JOIN_TAB *tab);
  bool make_aggr_tables_info();
  bool add_fields_for_current_rowid(JOIN_TAB *cur, List<Item> *fields);
  void init_join_cache_and_keyread();
};

enum enum_with_bush_roots { WITH_BUSH_ROOTS, WITHOUT_BUSH_ROOTS};
enum enum_with_const_tables { WITH_CONST_TABLES, WITHOUT_CONST_TABLES};

JOIN_TAB *first_linear_tab(JOIN *join,
                           enum enum_with_bush_roots include_bush_roots,
                           enum enum_with_const_tables const_tbls);
JOIN_TAB *next_linear_tab(JOIN* join, JOIN_TAB* tab, 
                          enum enum_with_bush_roots include_bush_roots);

JOIN_TAB *first_top_level_tab(JOIN *join, enum enum_with_const_tables with_const);
JOIN_TAB *next_top_level_tab(JOIN *join, JOIN_TAB *tab);

typedef struct st_select_check {
  uint const_ref,reg_ref;
} SELECT_CHECK;

extern const char *join_type_str[];

/* Extern functions in sql_select.cc */
void count_field_types(SELECT_LEX *select_lex, TMP_TABLE_PARAM *param, 
                       List<Item> &fields, bool reset_with_sum_func);
bool setup_copy_fields(THD *thd, TMP_TABLE_PARAM *param,
		       Ref_ptr_array ref_pointer_array,
		       List<Item> &new_list1, List<Item> &new_list2,
		       uint elements, List<Item> &fields);
void copy_fields(TMP_TABLE_PARAM *param);
bool copy_funcs(Item **func_ptr, const THD *thd);
uint find_shortest_key(TABLE *table, const key_map *usable_keys);
bool is_indexed_agg_distinct(JOIN *join, List<Item_field> *out_args);

/* functions from opt_sum.cc */
bool simple_pred(Item_func *func_item, Item **args, bool *inv_order);
int opt_sum_query(THD* thd,
                  List<TABLE_LIST> &tables, List<Item> &all_fields, COND *conds);

/* from sql_delete.cc, used by opt_range.cc */
extern "C" int refpos_order_cmp(void* arg, const void *a,const void *b);

/** class to copying an field/item to a key struct */

class store_key :public Sql_alloc
{
public:
  bool null_key; /* TRUE <=> the value of the key has a null part */
  enum store_key_result { STORE_KEY_OK, STORE_KEY_FATAL, STORE_KEY_CONV };
  enum Type { FIELD_STORE_KEY, ITEM_STORE_KEY, CONST_ITEM_STORE_KEY };
  store_key(THD *thd, Field *field_arg, uchar *ptr, uchar *null, uint length)
    :null_key(0), null_ptr(null), err(0)
  {
    to_field=field_arg->new_key_field(thd->mem_root, field_arg->table,
                                      ptr, length, null, 1);
  }
  store_key(store_key &arg)
    :Sql_alloc(), null_key(arg.null_key), to_field(arg.to_field),
             null_ptr(arg.null_ptr), err(arg.err)

  {}
  virtual ~store_key() {}			/** Not actually needed */
  virtual enum Type type() const=0;
  virtual const char *name() const=0;
  virtual bool store_key_is_const() { return false; }

  /**
    @brief sets ignore truncation warnings mode and calls the real copy method

    @details this function makes sure truncation warnings when preparing the
    key buffers don't end up as errors (because of an enclosing INSERT/UPDATE).
  */
  enum store_key_result copy(THD *thd)
  {
    enum store_key_result result;
    enum_check_fields org_count_cuted_fields= thd->count_cuted_fields;
    sql_mode_t org_sql_mode= thd->variables.sql_mode;
    thd->variables.sql_mode&= ~(MODE_NO_ZERO_IN_DATE | MODE_NO_ZERO_DATE);
    thd->variables.sql_mode|= MODE_INVALID_DATES;
    thd->count_cuted_fields= CHECK_FIELD_IGNORE;
    result= copy_inner();
    thd->count_cuted_fields= org_count_cuted_fields;
    thd->variables.sql_mode= org_sql_mode;
    return result;
  }

 protected:
  Field *to_field;				// Store data here
  uchar *null_ptr;
  uchar err;

  virtual enum store_key_result copy_inner()=0;
};


class store_key_field: public store_key
{
  Copy_field copy_field;
  const char *field_name;
 public:
  store_key_field(THD *thd, Field *to_field_arg, uchar *ptr,
                  uchar *null_ptr_arg,
		  uint length, Field *from_field, const char *name_arg)
    :store_key(thd, to_field_arg,ptr,
	       null_ptr_arg ? null_ptr_arg : from_field->maybe_null() ? &err
	       : (uchar*) 0, length), field_name(name_arg)
  {
    if (to_field)
    {
      copy_field.set(to_field,from_field,0);
    }
  }  

  enum Type type() const override { return FIELD_STORE_KEY; }
  const char *name() const override { return field_name; }

  void change_source_field(Item_field *fld_item)
  {
    copy_field.set(to_field, fld_item->field, 0);
    field_name= fld_item->full_name();
  }

 protected: 
  enum store_key_result copy_inner() override
  {
    TABLE *table= copy_field.to_field->table;
    MY_BITMAP *old_map= dbug_tmp_use_all_columns(table,
                                                 &table->write_set);

    /* 
      It looks like the next statement is needed only for a simplified
      hash function over key values used now in BNLH join.
      When the implementation of this function will be replaced for a proper
      full version this statement probably should be removed.
    */  
    bzero(copy_field.to_ptr,copy_field.to_length);

    copy_field.do_copy(&copy_field);
    dbug_tmp_restore_column_map(&table->write_set, old_map);
    null_key= to_field->is_null();
    return err != 0 ? STORE_KEY_FATAL : STORE_KEY_OK;
  }
};


class store_key_item :public store_key
{
 protected:
  Item *item;
  /*
    Flag that forces usage of save_val() method which save value of the
    item instead of save_in_field() method which saves result.
  */
  bool use_value;
public:
  store_key_item(THD *thd, Field *to_field_arg, uchar *ptr,
                 uchar *null_ptr_arg, uint length, Item *item_arg, bool val)
    :store_key(thd, to_field_arg, ptr,
	       null_ptr_arg ? null_ptr_arg : item_arg->maybe_null() ?
	       &err : (uchar*) 0, length), item(item_arg), use_value(val)
  {}
  store_key_item(store_key &arg, Item *new_item, bool val)
    :store_key(arg), item(new_item), use_value(val)
  {}


  enum Type type() const override { return ITEM_STORE_KEY; }
  const char *name() const  override { return "func"; }

 protected:  
  enum store_key_result copy_inner() override
  {
    TABLE *table= to_field->table;
    MY_BITMAP *old_map= dbug_tmp_use_all_columns(table,
                                                 &table->write_set);
    int res= FALSE;

    /* 
      It looks like the next statement is needed only for a simplified
      hash function over key values used now in BNLH join.
      When the implementation of this function will be replaced for a proper
      full version this statement probably should be removed.
    */  
    to_field->reset();

    if (use_value)
      item->save_val(to_field);
    else
      res= item->save_in_field(to_field, 1);
    /*
     Item::save_in_field() may call Item::val_xxx(). And if this is a subquery
     we need to check for errors executing it and react accordingly
    */
    if (!res && table->in_use->is_error())
      res= 1; /* STORE_KEY_FATAL */
    dbug_tmp_restore_column_map(&table->write_set, old_map);
    null_key= to_field->is_null() || item->null_value;
    return ((err != 0 || res < 0 || res > 2) ? STORE_KEY_FATAL : 
            (store_key_result) res);
  }
};


class store_key_const_item :public store_key_item
{
  bool inited;
public:
  store_key_const_item(THD *thd, Field *to_field_arg, uchar *ptr,
		       uchar *null_ptr_arg, uint length,
		       Item *item_arg)
    :store_key_item(thd, to_field_arg, ptr,
		    null_ptr_arg ? null_ptr_arg : item_arg->maybe_null() ?
		    &err : (uchar*) 0, length, item_arg, FALSE), inited(0)
  {
  }
  store_key_const_item(store_key &arg, Item *new_item)
    :store_key_item(arg, new_item, FALSE), inited(0)
  {}

  enum Type type() const override { return CONST_ITEM_STORE_KEY; }
  const char *name() const  override { return "const"; }
  bool store_key_is_const() override { return true; }

protected:  
  enum store_key_result copy_inner() override
  {
    int res;
    if (!inited)
    {
      inited=1;
      TABLE *table= to_field->table;
      MY_BITMAP *old_map= dbug_tmp_use_all_columns(table,
                                                   &table->write_set);
      if ((res= item->save_in_field(to_field, 1)))
      {       
        if (!err)
          err= res < 0 ? 1 : res; /* 1=STORE_KEY_FATAL */
      }
      /*
        Item::save_in_field() may call Item::val_xxx(). And if this is a subquery
        we need to check for errors executing it and react accordingly
        */
      if (!err && to_field->table->in_use->is_error())
        err= 1; /* STORE_KEY_FATAL */
      dbug_tmp_restore_column_map(&table->write_set, old_map);
    }
    null_key= to_field->is_null() || item->null_value;
    return (err > 2 ? STORE_KEY_FATAL : (store_key_result) err);
  }
};

void best_access_path(JOIN *join, JOIN_TAB *s,
                      table_map remaining_tables,
                      const POSITION *join_positions, uint idx,
                      bool disable_jbuf, double record_count,
                      POSITION *pos, POSITION *loose_scan_pos);
bool cp_buffer_from_ref(THD *thd, TABLE *table, TABLE_REF *ref);
bool error_if_full_join(JOIN *join);
int report_error(TABLE *table, int error);
int safe_index_read(JOIN_TAB *tab);
int get_quick_record(SQL_SELECT *select);
int setup_order(THD *thd, Ref_ptr_array ref_pointer_array, TABLE_LIST *tables,
                List<Item> &fields, List <Item> &all_fields, ORDER *order,
                bool from_window_spec= false);
int setup_group(THD *thd,  Ref_ptr_array ref_pointer_array, TABLE_LIST *tables,
		List<Item> &fields, List<Item> &all_fields, ORDER *order,
		bool *hidden_group_fields, bool from_window_spec= false);
bool fix_inner_refs(THD *thd, List<Item> &all_fields, SELECT_LEX *select,
                    Ref_ptr_array ref_pointer_array);
int join_read_key2(THD *thd, struct st_join_table *tab, TABLE *table,
                   struct st_table_ref *table_ref);

bool handle_select(THD *thd, LEX *lex, select_result *result,
                   ulong setup_tables_done_option);
bool mysql_select(THD *thd, TABLE_LIST *tables, List<Item> &list,
                  COND *conds, uint og_num, ORDER *order, ORDER *group,
                  Item *having, ORDER *proc_param, ulonglong select_type, 
                  select_result *result, SELECT_LEX_UNIT *unit, 
                  SELECT_LEX *select_lex);
void free_underlaid_joins(THD *thd, SELECT_LEX *select);
bool mysql_explain_union(THD *thd, SELECT_LEX_UNIT *unit,
                         select_result *result);

/*
  General routine to change field->ptr of a NULL-terminated array of Field
  objects. Useful when needed to call val_int, val_str or similar and the
  field data is not in table->record[0] but in some other structure.
  set_key_field_ptr changes all fields of an index using a key_info object.
  All methods presume that there is at least one field to change.
*/


class Virtual_tmp_table: public TABLE
{
  /**
    Destruct collected fields. This method can be called on errors,
    when we could not make the virtual temporary table completely,
    e.g. when some of the fields could not be created or added.

    This is needed to avoid memory leaks, as some fields can be BLOB
    variants and thus can have String onboard. Strings must be destructed
    as they store data on the heap (not on MEM_ROOT).
  */
  void destruct_fields()
  {
    for (uint i= 0; i < s->fields; i++)
    {
      field[i]->free();
      delete field[i];  // to invoke the field destructor
    }
    s->fields= 0;       // safety
  }

protected:
  /**
     The number of the fields that are going to be in the table.
     We remember the number of the fields at init() time, and
     at open() we check that all of the fields were really added.
  */
  uint m_alloced_field_count;

  /**
    Setup field pointers and null-bit pointers.
  */
  void setup_field_pointers();

public:
  /**
    Create a new empty virtual temporary table on the thread mem_root.
    After creation, the caller must:
    - call init()
    - populate the table with new fields using add().
    - call open().
    @param thd         - Current thread.
  */
  static void *operator new(size_t size, THD *thd) throw();
  static void operator delete(void *ptr, size_t size) { TRASH_FREE(ptr, size); }
  static void operator delete(void *, THD *) throw(){}

  Virtual_tmp_table(THD *thd) : m_alloced_field_count(0)
  {
    reset();
    temp_pool_slot= MY_BIT_NONE;
    in_use= thd;
    copy_blobs= true;
    alias.set("", 0, &my_charset_bin);
  }

  ~Virtual_tmp_table()
  {
    if (s)
      destruct_fields();
  }

  /**
    Allocate components for the given number of fields.
     - fields[]
     - s->blob_fields[],
     - bitmaps: def_read_set, def_write_set, tmp_set, eq_join_set, cond_set.
    @param field_count - The number of fields we plan to add to the table.
    @returns false     - on success.
    @returns true      - on error.
  */
  bool init(uint field_count);

  /**
    Add one Field to the end of the field array, update members:
    s->reclength, s->fields, s->blob_fields, s->null_fuelds.
  */
  bool add(Field *new_field)
  {
    DBUG_ASSERT(s->fields < m_alloced_field_count);
    new_field->init(this);
    field[s->fields]= new_field;
    s->reclength+= new_field->pack_length();
    if (!(new_field->flags & NOT_NULL_FLAG))
      s->null_fields++;
    if (new_field->flags & BLOB_FLAG)
    {
      // Note, s->blob_fields was incremented in Field_blob::Field_blob
      DBUG_ASSERT(s->blob_fields);
      DBUG_ASSERT(s->blob_fields <= m_alloced_field_count);
      s->blob_field[s->blob_fields - 1]= s->fields;
    }
    new_field->field_index= s->fields++;
    return false;
  }

  /**
    Add fields from a Spvar_definition list
    @returns false - on success.
    @returns true  - on error.
  */
  bool add(List<Spvar_definition> &field_list);

  /**
    Open a virtual table for read/write:
    - Setup end markers in TABLE::field and TABLE_SHARE::blob_fields,
    - Allocate a buffer in TABLE::record[0].
    - Set field pointers (Field::ptr, Field::null_pos, Field::null_bit) to
      the allocated record.
    This method is called when all of the fields have been added to the table.
    After calling this method the table is ready for read and write operations.
    @return false - on success
    @return true  - on error (e.g. could not allocate the record buffer).
  */
  bool open();

  void set_all_fields_to_null()
  {
    for (uint i= 0; i < s->fields; i++)
      field[i]->set_null();
  }
  /**
    Set all fields from a compatible item list.
    The number of fields in "this" must be equal to the number
    of elements in "value".
  */
  bool sp_set_all_fields_from_item_list(THD *thd, List<Item> &items);

  /**
    Set all fields from a compatible item.
    The number of fields in "this" must be the same with the number
    of elements in "value".
  */
  bool sp_set_all_fields_from_item(THD *thd, Item *value);

  /**
    Find a ROW element index by its name
    Assumes that "this" is used as a storage for a ROW-type SP variable.
    @param [OUT] idx        - the index of the found field is returned here
    @param [IN]  field_name - find a field with this name
    @return      true       - on error (the field was not found)
    @return      false      - on success (idx[0] was set to the field index)
  */
  bool sp_find_field_by_name(uint *idx, const LEX_CSTRING &name) const;

  /**
    Find a ROW element index by its name.
    If the element is not found, and error is issued.
    @param [OUT] idx        - the index of the found field is returned here
    @param [IN]  var_name   - the name of the ROW variable (for error reporting)
    @param [IN]  field_name - find a field with this name
    @return      true       - on error (the field was not found)
    @return      false      - on success (idx[0] was set to the field index)
  */
  bool sp_find_field_by_name_or_error(uint *idx,
                                      const LEX_CSTRING &var_name,
                                      const LEX_CSTRING &field_name) const;
};


/**
  Create a reduced TABLE object with properly set up Field list from a
  list of field definitions.

    The created table doesn't have a table handler associated with
    it, has no keys, no group/distinct, no copy_funcs array.
    The sole purpose of this TABLE object is to use the power of Field
    class to read/write data to/from table->record[0]. Then one can store
    the record in any container (RB tree, hash, etc).
    The table is created in THD mem_root, so are the table's fields.
    Consequently, if you don't BLOB fields, you don't need to free it.

  @param thd         connection handle
  @param field_list  list of column definitions

  @return
    0 if out of memory, or a
    TABLE object ready for read and write in case of success
*/

inline Virtual_tmp_table *
create_virtual_tmp_table(THD *thd, List<Spvar_definition> &field_list)
{
  Virtual_tmp_table *table;
  if (!(table= new(thd) Virtual_tmp_table(thd)))
    return NULL;

  /*
    If "simulate_create_virtual_tmp_table_out_of_memory" debug option
    is enabled, we now enable "simulate_out_of_memory". This effectively
    makes table->init() fail on OOM inside multi_alloc_root().
    This is done to test that ~Virtual_tmp_table() called from the "delete"
    below correcly handles OOM.
  */
  DBUG_EXECUTE_IF("simulate_create_virtual_tmp_table_out_of_memory",
                  DBUG_SET("+d,simulate_out_of_memory"););

  if (table->init(field_list.elements) ||
      table->add(field_list) ||
      table->open())
  {
    delete table;
    return NULL;
  }
  return table;
}


/**
  Create a new virtual temporary table consisting of a single field.
  SUM(DISTINCT expr) and similar numeric aggregate functions use this.
  @param thd    - Current thread
  @param field  - The field that will be added into the table.
  @return NULL  - On error.
  @return !NULL - A pointer to the created table that is ready
                  for read and write.
*/
inline TABLE *
create_virtual_tmp_table(THD *thd, Field *field)
{
  Virtual_tmp_table *table;
  DBUG_ASSERT(field);
  if (!(table= new(thd) Virtual_tmp_table(thd)))
    return NULL;
  if (table->init(1) ||
      table->add(field) ||
      table->open())
  {
    delete table;
    return NULL;
  }
  return table;
}


int test_if_item_cache_changed(List<Cached_item> &list);
int join_init_read_record(JOIN_TAB *tab);
void set_position(JOIN *join,uint idx,JOIN_TAB *table,KEYUSE *key);
inline Item * and_items(THD *thd, Item* cond, Item *item)
{
  return (cond ? (new (thd->mem_root) Item_cond_and(thd, cond, item)) : item);
}
inline Item * or_items(THD *thd, Item* cond, Item *item)
{
  return (cond ? (new (thd->mem_root) Item_cond_or(thd, cond, item)) : item);
}
bool choose_plan(JOIN *join, table_map join_tables);
void optimize_wo_join_buffering(JOIN *join, uint first_tab, uint last_tab, 
                                table_map last_remaining_tables, 
                                bool first_alt, uint no_jbuf_before,
                                double *outer_rec_count, double *reopt_cost);
Item_equal *find_item_equal(COND_EQUAL *cond_equal, Field *field,
                            bool *inherited_fl);
extern bool test_if_ref(Item *, 
                 Item_field *left_item,Item *right_item);

inline bool optimizer_flag(THD *thd, ulonglong flag)
{ 
  return (thd->variables.optimizer_switch & flag);
}

/*
int print_fake_select_lex_join(select_result_sink *result, bool on_the_fly,
                               SELECT_LEX *select_lex, uint8 select_options);
*/

uint get_index_for_order(ORDER *order, TABLE *table, SQL_SELECT *select,
                         ha_rows limit, ha_rows *scanned_limit, 
                         bool *need_sort, bool *reverse);
ORDER *simple_remove_const(ORDER *order, COND *where);
bool const_expression_in_where(COND *cond, Item *comp_item,
                               Field *comp_field= NULL,
                               Item **const_item= NULL);
bool cond_is_datetime_is_null(Item *cond);
bool cond_has_datetime_is_null(Item *cond);

/* Table elimination entry point function */
void eliminate_tables(JOIN *join);

/* Index Condition Pushdown entry point function */
void push_index_cond(JOIN_TAB *tab, uint keyno);

#define OPT_LINK_EQUAL_FIELDS    1

/* EXPLAIN-related utility functions */
int print_explain_message_line(select_result_sink *result, 
                               uint8 options, bool is_analyze,
                               uint select_number,
                               const char *select_type,
                               ha_rows *rows,
                               const char *message);
void explain_append_mrr_info(QUICK_RANGE_SELECT *quick, String *res);
int append_possible_keys(MEM_ROOT *alloc, String_list &list, TABLE *table, 
                         key_map possible_keys);
void unpack_to_base_table_fields(TABLE *table);

/****************************************************************************
  Temporary table support for SQL Runtime
 ***************************************************************************/

#define STRING_TOTAL_LENGTH_TO_PACK_ROWS 128
#define AVG_STRING_LENGTH_TO_PACK_ROWS   64
#define RATIO_TO_PACK_ROWS	       2
#define MIN_STRING_LENGTH_TO_PACK_ROWS   10

void calc_group_buffer(TMP_TABLE_PARAM *param, ORDER *group);
TABLE *create_tmp_table(THD *thd,TMP_TABLE_PARAM *param,List<Item> &fields,
			ORDER *group, bool distinct, bool save_sum_fields,
			ulonglong select_options, ha_rows rows_limit,
                        const LEX_CSTRING *alias, bool do_not_open=FALSE,
                        bool keep_row_order= FALSE);
TABLE *create_tmp_table_for_schema(THD *thd, TMP_TABLE_PARAM *param,
                                   const ST_SCHEMA_TABLE &schema_table,
                                   longlong select_options,
                                   const LEX_CSTRING &alias,
                                   bool do_not_open, bool keep_row_order);

void free_tmp_table(THD *thd, TABLE *entry);
bool create_internal_tmp_table_from_heap(THD *thd, TABLE *table,
                                         TMP_ENGINE_COLUMNDEF *start_recinfo,
                                         TMP_ENGINE_COLUMNDEF **recinfo, 
                                         int error, bool ignore_last_dupp_key_error,
                                         bool *is_duplicate);
bool create_internal_tmp_table(TABLE *table, KEY *keyinfo, 
                               TMP_ENGINE_COLUMNDEF *start_recinfo,
                               TMP_ENGINE_COLUMNDEF **recinfo, 
                               ulonglong options);
bool instantiate_tmp_table(TABLE *table, KEY *keyinfo, 
                           TMP_ENGINE_COLUMNDEF *start_recinfo,
                           TMP_ENGINE_COLUMNDEF **recinfo,
                           ulonglong options);
bool open_tmp_table(TABLE *table);
double prev_record_reads(const POSITION *positions, uint idx, table_map found_ref);
void fix_list_after_tbl_changes(SELECT_LEX *new_parent, List<TABLE_LIST> *tlist);
double get_tmp_table_lookup_cost(THD *thd, double row_count, uint row_size);
double get_tmp_table_write_cost(THD *thd, double row_count, uint row_size);
void optimize_keyuse(JOIN *join, DYNAMIC_ARRAY *keyuse_array);
bool sort_and_filter_keyuse(THD *thd, DYNAMIC_ARRAY *keyuse,
                            bool skip_unprefixed_keyparts);

struct st_cond_statistic
{
  Item *cond;
  Field *field_arg;
  ulong positive;
};
typedef struct st_cond_statistic COND_STATISTIC;

ulong check_selectivity(THD *thd,
                        ulong rows_to_read,
                        TABLE *table,
                        List<COND_STATISTIC> *conds);

class Pushdown_query: public Sql_alloc
{
public:
  SELECT_LEX *select_lex;
  bool store_data_in_temp_table;
  group_by_handler *handler;
  Item *having;

  Pushdown_query(SELECT_LEX *select_lex_arg, group_by_handler *handler_arg)
    : select_lex(select_lex_arg), store_data_in_temp_table(0),
    handler(handler_arg), having(0) {}

  ~Pushdown_query() { delete handler; }

  /* Function that calls the above scan functions */
  int execute(JOIN *);
};

class derived_handler;

class Pushdown_derived: public Sql_alloc
{
private:
  bool is_analyze;
public:
  TABLE_LIST *derived;
  derived_handler *handler;

  Pushdown_derived(TABLE_LIST *tbl, derived_handler *h);

  ~Pushdown_derived();

  int execute(); 
};


class select_handler;


bool test_if_order_compatible(SQL_I_List<ORDER> &a, SQL_I_List<ORDER> &b);
int test_if_group_changed(List<Cached_item> &list);
int create_sort_index(THD *thd, JOIN *join, JOIN_TAB *tab, Filesort *fsort);

JOIN_TAB *first_explain_order_tab(JOIN* join);
JOIN_TAB *next_explain_order_tab(JOIN* join, JOIN_TAB* tab);

bool is_eliminated_table(table_map eliminated_tables, TABLE_LIST *tbl);

bool check_simple_equality(THD *thd, const Item::Context &ctx,
                           Item *left_item, Item *right_item,
                           COND_EQUAL *cond_equal);

void propagate_new_equalities(THD *thd, Item *cond,
                              List<Item_equal> *new_equalities,
                              COND_EQUAL *inherited,
                              bool *is_simplifiable_cond);

bool dbug_user_var_equals_str(THD *thd, const char *name, const char *value);
#endif /* SQL_SELECT_INCLUDED */
