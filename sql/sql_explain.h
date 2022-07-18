/*
   Copyright (c) 2013 Monty Program Ab

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1335 USA */

/*

== EXPLAIN/ANALYZE architecture ==

=== [SHOW] EXPLAIN data ===
Query optimization produces two data structures:
1. execution data structures themselves (eg. JOINs, JOIN_TAB, etc, etc)
2. Explain data structures.

#2 are self contained set of data structures that has sufficient info to
produce output of SHOW EXPLAIN, EXPLAIN [FORMAT=JSON], or 
ANALYZE [FORMAT=JSON], without accessing the execution data structures.

The exception is that Explain data structures have Item* pointers. See
ExplainDataStructureLifetime below for details.

=== ANALYZE data ===
EXPLAIN data structures have embedded ANALYZE data structures. These are 
objects that are used to track how the parts of query plan were executed:
how many times each part of query plan was invoked, how many rows were
read/returned, etc.

Each execution data structure keeps a direct pointer to its ANALYZE data
structure. It is needed so that execution code can quickly increment the
counters.

(note that this increases the set of data that is frequently accessed 
during the execution. What is the impact of this?)

Since ANALYZE/EXPLAIN data structures are separated from execution data
structures, it is easy to have them survive until the end of the query,
where we can return ANALYZE [FORMAT=JSON] output to the user, or print 
it into the slow query log.

*/

#ifndef SQL_EXPLAIN_INCLUDED
#define SQL_EXPLAIN_INCLUDED

class String_list: public List<char>
{
public:
  const char *append_str(MEM_ROOT *mem_root, const char *str);
};

class Json_writer;

/**************************************************************************************
 
  Data structures for producing EXPLAIN outputs.

  These structures
  - Can be produced inexpensively from query plan.
  - Store sufficient information to produce tabular EXPLAIN output (the goal is 
    to be able to produce JSON also)

*************************************************************************************/



class Explain_query;

/* 
  A node can be either a SELECT, or a UNION.
*/
class Explain_node : public Sql_alloc
{
public:
  Explain_node(MEM_ROOT *root) :
    cache_tracker(NULL),
    connection_type(EXPLAIN_NODE_OTHER),
    children(root)
  {}
  /* A type specifying what kind of node this is */
  enum explain_node_type 
  {
    EXPLAIN_UNION, 
    EXPLAIN_SELECT,
    EXPLAIN_BASIC_JOIN,
    EXPLAIN_UPDATE,
    EXPLAIN_DELETE, 
    EXPLAIN_INSERT
  };
  
  /* How this node is connected */
  enum explain_connection_type {
    EXPLAIN_NODE_OTHER,
    EXPLAIN_NODE_DERIVED, /* Materialized derived table */
    EXPLAIN_NODE_NON_MERGED_SJ /* aka JTBM semi-join */
  };

  virtual enum explain_node_type get_type()= 0;
  virtual uint get_select_id()= 0;

  /**
    expression cache statistics
  */
  Expression_cache_tracker* cache_tracker;

  /*
    How this node is connected to its parent.
    (NOTE: EXPLAIN_NODE_NON_MERGED_SJ is set very late currently)
  */
  enum explain_connection_type connection_type;

protected:
  /* 
    A node may have children nodes. When a node's explain structure is 
    created, children nodes may not yet have QPFs. This is why we store ids.
  */
  Dynamic_array<int> children;
public:
  void add_child(int select_no)
  {
    children.append(select_no);
  }

  virtual int print_explain(Explain_query *query, select_result_sink *output, 
                            uint8 explain_flags, bool is_analyze)=0;
  virtual void print_explain_json(Explain_query *query, Json_writer *writer, 
                                  bool is_analyze, bool no_tmp_tbl)= 0;

  int print_explain_for_children(Explain_query *query, select_result_sink *output, 
                                 uint8 explain_flags, bool is_analyze);
  void print_explain_json_for_children(Explain_query *query,
                                       Json_writer *writer, bool is_analyze,
                                       bool no_tmp_tbl);
  bool print_explain_json_cache(Json_writer *writer, bool is_analyze);
  virtual ~Explain_node(){}
};


class Explain_table_access;


/* 
  A basic join. This is only used for SJ-Materialization nests.

  Basic join doesn't have ORDER/GROUP/DISTINCT operations. It also cannot be
  degenerate.

  It has its own select_id.
*/
class Explain_basic_join : public Explain_node
{
public:
  enum explain_node_type get_type() { return EXPLAIN_BASIC_JOIN; }
  
  Explain_basic_join(MEM_ROOT *root) : Explain_node(root), join_tabs(NULL) {}
  ~Explain_basic_join();

  bool add_table(Explain_table_access *tab, Explain_query *query);

  uint get_select_id() { return select_id; }

  uint select_id;

  int print_explain(Explain_query *query, select_result_sink *output,
                    uint8 explain_flags, bool is_analyze);
  void print_explain_json(Explain_query *query, Json_writer *writer, 
                          bool is_analyze, bool no_tmp_tbl);

  void print_explain_json_interns(Explain_query *query, Json_writer *writer,
                                  bool is_analyze, bool no_tmp_tbl);

  /* A flat array of Explain structs for tables. */
  Explain_table_access** join_tabs;
  uint n_join_tabs;
};


class Explain_aggr_node;
/*
  EXPLAIN structure for a SELECT.
  
  A select can be:
  1. A degenerate case. In this case, message!=NULL, and it contains a 
     description of what kind of degenerate case it is (e.g. "Impossible 
     WHERE").
  2. a non-degenrate join. In this case, join_tabs describes the join.

  In the non-degenerate case, a SELECT may have a GROUP BY/ORDER BY operation.

  In both cases, the select may have children nodes. class Explain_node
  provides a way get node's children.
*/

class Explain_select : public Explain_basic_join
{
public:
  enum explain_node_type get_type() { return EXPLAIN_SELECT; }

  Explain_select(MEM_ROOT *root, bool is_analyze) : 
  Explain_basic_join(root),
#ifndef DBUG_OFF
    select_lex(NULL),
#endif
    linkage(UNSPECIFIED_TYPE),
    is_lateral(false),
    message(NULL),
    having(NULL), having_value(Item::COND_UNDEF),
    using_temporary(false), using_filesort(false),
    time_tracker(is_analyze),
    aggr_tree(NULL)
  {}

  void add_linkage(Json_writer *writer);

public:
#ifndef DBUG_OFF
  SELECT_LEX *select_lex;
#endif
  const char *select_type;
  enum sub_select_type linkage;
  bool is_lateral;

  /*
    If message != NULL, this is a degenerate join plan, and all subsequent
    members have no info 
  */
  const char *message;

  /* Expensive constant condition */
  Item *exec_const_cond;
  Item *outer_ref_cond;
  Item *pseudo_bits_cond;

  /* HAVING condition */
  Item *having;
  Item::cond_result having_value;

  /* Global join attributes. In tabular form, they are printed on the first row */
  bool using_temporary;
  bool using_filesort;

  /* ANALYZE members */
  Time_and_counter_tracker time_tracker;
  
  /* 
    Part of query plan describing sorting, temp.table usage, and duplicate 
    removal
  */
  Explain_aggr_node* aggr_tree;

  int print_explain(Explain_query *query, select_result_sink *output, 
                    uint8 explain_flags, bool is_analyze);
  void print_explain_json(Explain_query *query, Json_writer *writer, 
                          bool is_analyze, bool no_tmp_tbl);
  
  Table_access_tracker *get_using_temporary_read_tracker()
  {
    return &using_temporary_read_tracker;
  }
private:
  Table_access_tracker using_temporary_read_tracker;
};

/////////////////////////////////////////////////////////////////////////////
// EXPLAIN structures for ORDER/GROUP operations.
/////////////////////////////////////////////////////////////////////////////
typedef enum 
{
  AGGR_OP_TEMP_TABLE,
  AGGR_OP_FILESORT,
  //AGGR_OP_READ_SORTED_FILE, // need this?
  AGGR_OP_REMOVE_DUPLICATES,
  AGGR_OP_WINDOW_FUNCS
  //AGGR_OP_JOIN // Need this?
} enum_explain_aggr_node_type;


class Explain_aggr_node : public Sql_alloc
{
public:
  virtual enum_explain_aggr_node_type get_type()= 0;
  virtual ~Explain_aggr_node() {}
  Explain_aggr_node *child;
};

class Explain_aggr_filesort : public Explain_aggr_node
{
  List<Item> sort_items;
  List<ORDER::enum_order> sort_directions;
public:
  enum_explain_aggr_node_type get_type() { return AGGR_OP_FILESORT; }
  Filesort_tracker tracker;

  Explain_aggr_filesort(MEM_ROOT *mem_root, bool is_analyze, 
                        Filesort *filesort);

  void print_json_members(Json_writer *writer, bool is_analyze,
                          bool no_tmp_tbl);
};

class Explain_aggr_tmp_table : public Explain_aggr_node
{
public:
  enum_explain_aggr_node_type get_type() { return AGGR_OP_TEMP_TABLE; }
};

class Explain_aggr_remove_dups : public Explain_aggr_node
{
public:
  enum_explain_aggr_node_type get_type() { return AGGR_OP_REMOVE_DUPLICATES; }
};

class Explain_aggr_window_funcs : public Explain_aggr_node
{
  List<Explain_aggr_filesort> sorts;
public:
  enum_explain_aggr_node_type get_type() { return AGGR_OP_WINDOW_FUNCS; }

  void print_json_members(Json_writer *writer, bool is_analyze,
                          bool no_tmp_tbl);
  friend class Window_funcs_computation;
};

/////////////////////////////////////////////////////////////////////////////

extern const char *unit_operation_text[4];
extern const char *pushed_derived_text;
extern const char *pushed_select_text;

/*
  Explain structure for a UNION [ALL].

  A UNION may or may not have "Using filesort".
*/

class Explain_union : public Explain_node
{
public:
  Explain_union(MEM_ROOT *root, bool is_analyze) : 
    Explain_node(root), union_members(PSI_INSTRUMENT_MEM),
    is_recursive_cte(false),
    fake_select_lex_explain(root, is_analyze)
  {}

  enum explain_node_type get_type() { return EXPLAIN_UNION; }
  unit_common_op operation;

  uint get_select_id()
  {
    DBUG_ASSERT(union_members.elements() > 0);
    return union_members.at(0);
  }
  /*
    Members of the UNION.  Note: these are different from UNION's "children".
    Example:

      (select * from t1) union 
      (select * from t2) order by (select col1 from t3 ...)

    here 
      - select-from-t1 and select-from-t2 are "union members",
      - select-from-t3 is the only "child".
  */
  Dynamic_array<int> union_members;

  void add_select(int select_no)
  {
    union_members.append(select_no);
  }
  int print_explain(Explain_query *query, select_result_sink *output, 
                    uint8 explain_flags, bool is_analyze);
  void print_explain_json(Explain_query *query, Json_writer *writer, 
                          bool is_analyze, bool no_tmp_tbl);

  const char *fake_select_type;
  bool using_filesort;
  bool using_tmp;
  bool is_recursive_cte;
  
  /*
    Explain data structure for "fake_select_lex" (i.e. for the degenerate
    SELECT that reads UNION result).
    It doesn't have a query plan, but we still need execution tracker, etc.
  */
  Explain_select fake_select_lex_explain;

  Table_access_tracker *get_fake_select_lex_tracker()
  {
    return &fake_select_lex_tracker;
  }
  Table_access_tracker *get_tmptable_read_tracker()
  {
    return &tmptable_read_tracker;
  }
private:
  uint make_union_table_name(char *buf);
  
  Table_access_tracker fake_select_lex_tracker;
  /* This one is for reading after ORDER BY */
  Table_access_tracker tmptable_read_tracker; 
};


class Explain_update;
class Explain_delete;
class Explain_insert;


/*
  Explain structure for a query (i.e. a statement).

  This should be able to survive when the query plan was deleted. Currently,
  we do not intend for it survive until after query's MEM_ROOT is freed.

  == ExplainDataStructureLifetime ==

    >dispatch_command
    | >mysql_parse
    | | ...
    | |
    | | explain->query_plan_ready(); // (1)
    | |
    | |   some_join->cleanup(); //  (2)
    | |
    | | explain->notify_tables_are_closed(); // (3)
    | | close_thread_tables();  // (4)
    | | ...
    | | free_items(); // (5)
    | | ...
    | |
    | <mysql_parse
    |
    | log_slow_statement() // (6)
    |
    | free_root()
    |
    >dispatch_command

  (1) - Query plan construction is finished and it is available for reading.

  (2) - Temporary tables are freed. After this point,
        we need to pass QT_DONT_ACCESS_TMP_TABLES to item->print(). Since
        we don't track when #2 happens for each temp.table, we pass this
        flag whenever we're printing the query plan for a SHOW command.
        Also, we pass it when printing ANALYZE (?)

  (3) - Notification about (4).
  (4) - Tables used by the query are closed. One known consequence of this is
        that the values of the const tables' fields are not available anymore.
        We could use the same approach as in QT_DONT_ACCESS_TMP_TABLES to work
        around that, but instead we disallow producing FORMAT=JSON output at
        step #3. We also processing of SHOW command. The rationale is that
        query is close to finish anyway.

  (5) - Item objects are freed. After this, it's certainly not possible to
        print them into FORMAT=JSON output.

  (6) - We may decide to log tabular EXPLAIN output to the slow query log.

*/

class Explain_query : public Sql_alloc
{
public:
  Explain_query(THD *thd, MEM_ROOT *root);
  ~Explain_query();

  /* Add a new node */
  void add_node(Explain_node *node);
  void add_insert_plan(Explain_insert *insert_plan_arg);
  void add_upd_del_plan(Explain_update *upd_del_plan_arg);

  /* This will return a select, or a union */
  Explain_node *get_node(uint select_id);

  /* This will return a select (even if there is a union with this id) */
  Explain_select *get_select(uint select_id);
  
  Explain_union *get_union(uint select_id);
 
  /* Produce a tabular EXPLAIN output */
  int print_explain(select_result_sink *output, uint8 explain_flags, 
                    bool is_analyze);
  
  /* Send tabular EXPLAIN to the client */
  int send_explain(THD *thd);
  
  /* Return tabular EXPLAIN output as a text string */
  bool print_explain_str(THD *thd, String *out_str, bool is_analyze);

  int print_explain_json(select_result_sink *output, bool is_analyze,
                         bool is_show_cmd,
                         ulonglong query_time_in_progress_ms= 0);

  /* If true, at least part of EXPLAIN can be printed */
  bool have_query_plan() { return insert_plan || upd_del_plan|| get_node(1) != NULL; }

  void query_plan_ready();
  void notify_tables_are_closed();

  MEM_ROOT *mem_root;

  Explain_update *get_upd_del_plan() { return upd_del_plan; }
private:
  bool print_query_blocks_json(Json_writer *writer, const bool is_analyze, const bool is_show_cmd);
  void print_query_optimization_json(Json_writer *writer);
  void send_explain_json_to_output(Json_writer *writer, select_result_sink *output);
 
  /* Explain_delete inherits from Explain_update */
  Explain_update *upd_del_plan;

  /* Query "plan" for INSERTs */
  Explain_insert *insert_plan;

  Dynamic_array<Explain_union*> unions;
  Dynamic_array<Explain_select*> selects;
  
  THD *stmt_thd; // for APC start/stop
  bool apc_enabled;
  /* 
    Debugging aid: count how many times add_node() was called. Ideally, it
    should be one, we currently allow O(1) query plan saves for each
    select or union.  The goal is not to have O(#rows_in_some_table), which 
    is unacceptable.
  */
  longlong operations;
#ifndef DBUG_OFF
  bool can_print_json= false;
#endif

  Exec_time_tracker optimization_time_tracker;
};


/* 
  Some of the tags have matching text. See extra_tag_text for text names, and 
  Explain_table_access::append_tag_name() for code to convert from tag form to text
  form.
*/
enum explain_extra_tag
{
  ET_none= 0, /* not-a-tag */
  ET_USING_INDEX_CONDITION,
  ET_USING_INDEX_CONDITION_BKA,
  ET_USING, /* For quick selects of various kinds */
  ET_RANGE_CHECKED_FOR_EACH_RECORD,
  ET_USING_WHERE_WITH_PUSHED_CONDITION,
  ET_USING_WHERE,
  ET_NOT_EXISTS,

  ET_USING_INDEX,
  ET_FULL_SCAN_ON_NULL_KEY,
  ET_SKIP_OPEN_TABLE,
  ET_OPEN_FRM_ONLY,
  ET_OPEN_FULL_TABLE,

  ET_SCANNED_0_DATABASES,
  ET_SCANNED_1_DATABASE,
  ET_SCANNED_ALL_DATABASES,

  ET_USING_INDEX_FOR_GROUP_BY,

  ET_USING_MRR, // does not print "Using mrr". 

  ET_DISTINCT,
  ET_LOOSESCAN,
  ET_START_TEMPORARY,
  ET_END_TEMPORARY,
  ET_FIRST_MATCH,
  
  ET_USING_JOIN_BUFFER,

  ET_CONST_ROW_NOT_FOUND,
  ET_UNIQUE_ROW_NOT_FOUND,
  ET_IMPOSSIBLE_ON_CONDITION,
  ET_TABLE_FUNCTION,

  ET_total
};


/*
  Explain data structure describing join buffering use.
*/

class EXPLAIN_BKA_TYPE
{
public:
  EXPLAIN_BKA_TYPE() : join_alg(NULL) {}

  size_t join_buffer_size;

  bool incremental;

  /* 
    NULL if no join buferring used.
    Other values: BNL, BNLH, BKA, BKAH.
  */
  const char *join_alg;

  /* Information about MRR usage.  */
  StringBuffer<64> mrr_type;
  
  bool is_using_jbuf() { return (join_alg != NULL); }
};


/*
  Data about how an index is used by some access method
*/
class Explain_index_use : public Sql_alloc
{
  char *key_name;
  uint key_len;
  char *filter_name;
  uint filter_len;
public:
  String_list key_parts_list;
  
  Explain_index_use()
  {
    clear();
  }

  void clear()
  {
    key_name= NULL;
    key_len= (uint)-1;
    filter_name= NULL;
    filter_len= (uint)-1;
  }
  bool set(MEM_ROOT *root, KEY *key_name, uint key_len_arg);
  bool set_pseudo_key(MEM_ROOT *root, const char *key_name);

  inline const char *get_key_name() const { return key_name; }
  inline uint get_key_len() const { return key_len; }
  //inline const char *get_filter_name() const { return filter_name; }
};


/*
  Query Plan data structure for Rowid filter.
*/
class Explain_rowid_filter : public Sql_alloc
{
public:
  /* Quick select used to collect the rowids into filter */
  Explain_quick_select *quick;

  /* How many rows the above quick select is expected to return */
  ha_rows rows;

  /* Expected selectivity for the filter */
  double selectivity;

  /* Tracker with the information about how rowid filter is executed */
  Rowid_filter_tracker *tracker;

  void print_explain_json(Explain_query *query, Json_writer *writer,
                          bool is_analyze);

  /*
    TODO:
      Here should be ANALYZE members:
      - r_rows for the quick select
      - An object that tracked the table access time
      - real selectivity of the filter.
  */
};


/*
  QPF for quick range selects, as well as index_merge select
*/
class Explain_quick_select : public Sql_alloc
{
public:
  Explain_quick_select(int quick_type_arg) : quick_type(quick_type_arg) 
  {}

  const int quick_type;

  bool is_basic() 
  {
    return (quick_type == QUICK_SELECT_I::QS_TYPE_RANGE || 
            quick_type == QUICK_SELECT_I::QS_TYPE_RANGE_DESC ||
            quick_type == QUICK_SELECT_I::QS_TYPE_GROUP_MIN_MAX);
  }
  
  /* This is used when quick_type == QUICK_SELECT_I::QS_TYPE_RANGE */
  Explain_index_use range;
  
  /* Used in all other cases */
  List<Explain_quick_select> children;
  
  void print_extra(String *str);
  void print_key(String *str);
  void print_key_len(String *str);

  void print_json(Json_writer *writer);

  void print_extra_recursive(String *str);
private:
  const char *get_name_by_type();
};


/*
  Data structure for "range checked for each record". 
  It's a set of keys, tabular explain prints hex bitmap, json prints key names.
*/

typedef const char* NAME;

class Explain_range_checked_fer : public Sql_alloc
{
public:
  String_list key_set;
  key_map keys_map;
private:
  ha_rows full_scan, index_merge;
  ha_rows *keys_stat;
  NAME *keys_stat_names;
  uint keys;

public:
  Explain_range_checked_fer()
    :Sql_alloc(), full_scan(0), index_merge(0),
    keys_stat(0), keys_stat_names(0), keys(0)
  {}

  int append_possible_keys_stat(MEM_ROOT *alloc,
                                TABLE *table, key_map possible_keys);
  void collect_data(QUICK_SELECT_I *quick);
  void print_json(Json_writer *writer, bool is_analyze);
};


/*
  EXPLAIN data structure for a single JOIN_TAB.
*/

class Explain_table_access : public Sql_alloc
{
public:
  Explain_table_access(MEM_ROOT *root) :
    derived_select_number(0),
    non_merged_sjm_number(0),
    extra_tags(root),
    range_checked_fer(NULL),
    full_scan_on_null_key(false),
    start_dups_weedout(false),
    end_dups_weedout(false),
    where_cond(NULL),
    cache_cond(NULL),
    pushed_index_cond(NULL),
    sjm_nest(NULL),
    pre_join_sort(NULL),
    rowid_filter(NULL)
  {}
  ~Explain_table_access() { delete sjm_nest; }

  void push_extra(enum explain_extra_tag extra_tag);

  /* Internals */

  /* id and 'select_type' are cared-of by the parent Explain_select */
  StringBuffer<32> table_name;
  StringBuffer<32> used_partitions;
  String_list used_partitions_list;
  // valid with ET_USING_MRR
  StringBuffer<32> mrr_type;
  StringBuffer<32> firstmatch_table_name;

  /* 
    Non-zero number means this is a derived table. The number can be used to
    find the query plan for the derived table
  */
  int derived_select_number;
  /* TODO: join with the previous member. */
  int non_merged_sjm_number;

  enum join_type type;

  bool used_partitions_set;
  
  /* Empty means "NULL" will be printed */
  String_list possible_keys;

  bool rows_set; /* not set means 'NULL' should be printed */
  bool filtered_set; /* not set means 'NULL' should be printed */
  // Valid if ET_USING_INDEX_FOR_GROUP_BY is present
  bool loose_scan_is_scanning;
  
  /*
    Index use: key name and length.
    Note: that when one is accessing I_S tables, those may show use of 
    non-existant indexes.

    key.key_name == NULL means 'NULL' will be shown in tabular output.
    key.key_len == (uint)-1 means 'NULL' will be shown in tabular output.
  */
  Explain_index_use key;
  
  /*
    when type==JT_HASH_NEXT, 'key' stores the hash join pseudo-key.
    hash_next_key stores the table's key.
  */
  Explain_index_use hash_next_key;
  
  String_list ref_list;

  ha_rows rows;
  double filtered;

  /* 
    Contents of the 'Extra' column. Some are converted into strings, some have
    parameters, values for which are stored below.
  */
  Dynamic_array<enum explain_extra_tag> extra_tags;

  // Valid if ET_USING tag is present
  Explain_quick_select *quick_info;
  
  /* Non-NULL value means this tab uses "range checked for each record" */
  Explain_range_checked_fer *range_checked_fer;
 
  bool full_scan_on_null_key;

  // valid with ET_USING_JOIN_BUFFER
  EXPLAIN_BKA_TYPE bka_type;

  bool start_dups_weedout;
  bool end_dups_weedout;
  
  /*
    Note: lifespan of WHERE condition is less than lifespan of this object.
    The below two are valid if tags include "ET_USING_WHERE".
    (TODO: indexsubquery may put ET_USING_WHERE without setting where_cond?)
  */
  Item *where_cond;
  Item *cache_cond;
  
  /*
    This is either pushed index condition, or BKA's index condition. 
    (the latter refers to columns of other tables and so can only be checked by
     BKA code). Examine extra_tags to tell which one it is.
  */
  Item *pushed_index_cond;

  Explain_basic_join *sjm_nest;
  
  /*
    This describes a possible filesort() call that is done before doing the
    join operation.
  */
  Explain_aggr_filesort *pre_join_sort;

  /* ANALYZE members */

  /* Tracker for reading the table */
  Table_access_tracker tracker;
  Exec_time_tracker op_tracker;
  Gap_time_tracker extra_time_tracker;

  Table_access_tracker jbuf_tracker;
  
  Explain_rowid_filter *rowid_filter;

  int print_explain(select_result_sink *output, uint8 explain_flags, 
                    bool is_analyze,
                    uint select_id, const char *select_type,
                    bool using_temporary, bool using_filesort);
  void print_explain_json(Explain_query *query, Json_writer *writer,
                          bool is_analyze, bool no_tmp_tbl);

private:
  void append_tag_name(String *str, enum explain_extra_tag tag);
  void fill_key_str(String *key_str, bool is_json) const;
  void fill_key_len_str(String *key_len_str, bool is_json) const;
  double get_r_filtered();
  void tag_to_json(Json_writer *writer, enum explain_extra_tag tag,
                   bool no_tmp_tbl);
};


/*
  EXPLAIN structure for single-table UPDATE. 
  
  This is similar to Explain_table_access, except that it is more restrictive.
  Also, it can have UPDATE operation options, but currently there aren't any.

  Explain_delete inherits from this.
*/

class Explain_update : public Explain_node
{
public:

  Explain_update(MEM_ROOT *root, bool is_analyze) : 
    Explain_node(root),
    filesort_tracker(NULL),
    command_tracker(is_analyze)
  {}

  virtual enum explain_node_type get_type() { return EXPLAIN_UPDATE; }
  virtual uint get_select_id() { return 1; /* always root */ }

  const char *select_type;

  StringBuffer<32> used_partitions;
  String_list used_partitions_list;
  bool used_partitions_set;

  bool impossible_where;
  bool no_partitions;
  StringBuffer<64> table_name;

  enum join_type jtype;
  String_list possible_keys;

  /* Used key when doing a full index scan (possibly with limit) */
  Explain_index_use key;

  /* 
    MRR that's used with quick select. This should probably belong to the
    quick select
  */
  StringBuffer<64> mrr_type;
  
  Explain_quick_select *quick_info;

  bool using_where;
  Item *where_cond;

  ha_rows rows;

  bool using_io_buffer;

  /* Tracker for doing reads when filling the buffer */
  Table_access_tracker buf_tracker;
  
  bool is_using_filesort() { return filesort_tracker? true: false; }
  /*
    Non-null value of filesort_tracker means "using filesort"

    if we are using filesort, then table_tracker is for the io done inside
    filesort.
    
    'tracker' is for tracking post-filesort reads.
  */
  Filesort_tracker *filesort_tracker;

  /* ANALYZE members and methods */
  Table_access_tracker tracker;

  /* This tracks execution of the whole command */
  Time_and_counter_tracker command_tracker;
  
  /* TODO: This tracks time to read rows from the table */
  Exec_time_tracker table_tracker;

  virtual int print_explain(Explain_query *query, select_result_sink *output, 
                            uint8 explain_flags, bool is_analyze);
  virtual void print_explain_json(Explain_query *query, Json_writer *writer,
                                  bool is_analyze, bool no_tmp_tbl);
};


/*
  EXPLAIN data structure for an INSERT.
  
  At the moment this doesn't do much as we don't really have any query plans
  for INSERT statements.
*/

class Explain_insert : public Explain_node
{
public:
  Explain_insert(MEM_ROOT *root) : 
  Explain_node(root)
  {}

  StringBuffer<64> table_name;

  enum explain_node_type get_type() { return EXPLAIN_INSERT; }
  uint get_select_id() { return 1; /* always root */ }

  int print_explain(Explain_query *query, select_result_sink *output, 
                    uint8 explain_flags, bool is_analyze);
  void print_explain_json(Explain_query *query, Json_writer *writer, 
                          bool is_analyze, bool no_tmp_tbl);
};


/* 
  EXPLAIN data of a single-table DELETE.
*/

class Explain_delete: public Explain_update
{
public:
  Explain_delete(MEM_ROOT *root, bool is_analyze) : 
  Explain_update(root, is_analyze)
  {}

  /*
    TRUE means we're going to call handler->delete_all_rows() and not read any
    rows.
  */
  bool deleting_all_rows;

  virtual enum explain_node_type get_type() { return EXPLAIN_DELETE; }
  virtual uint get_select_id() { return 1; /* always root */ }

  virtual int print_explain(Explain_query *query, select_result_sink *output, 
                            uint8 explain_flags, bool is_analyze);
  virtual void print_explain_json(Explain_query *query, Json_writer *writer,
                                  bool is_analyze, bool no_tmp_tbl);
};


#endif //SQL_EXPLAIN_INCLUDED
