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
   Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA */


class String_list: public List<char>
{
public:
  bool append_str(MEM_ROOT *mem_root, const char *str);
};



/* Data structures for ANALYZE */
class Table_access_tracker 
{
public:
  Table_access_tracker() :
    r_scans(0), r_rows(0), /*r_rows_after_table_cond(0),*/
    r_rows_after_where(0)
  {}

  ha_rows r_scans; /* How many scans were ran on this join_tab */
  ha_rows r_rows; /* How many rows we've got after that */
//  ha_rows r_rows_after_table_cond; /* Rows after applying the table condition */
  ha_rows r_rows_after_where; /* Rows after applying attached part of WHERE */

  bool has_scans() { return (r_scans != 0); }
  ha_rows get_avg_rows()
  {
    return r_scans ? (ha_rows)rint((double) r_rows / r_scans): 0;
  }

  double get_filtered_after_where()
  {
    double r_filtered;
    if (r_rows > 0)
      r_filtered= (double)r_rows_after_where / r_rows;
    else
      r_filtered= 1.0;

    return r_filtered;
  }
  
  inline void on_scan_init() { r_scans++; }
  inline void on_record_read() { r_rows++; }
  inline void on_record_after_where() { r_rows_after_where++; }
};


/**************************************************************************************
 
  Data structures for producing EXPLAIN outputs.

  These structures
  - Can be produced inexpensively from query plan.
  - Store sufficient information to produce tabular EXPLAIN output (the goal is 
    to be able to produce JSON also)

*************************************************************************************/


const int FAKE_SELECT_LEX_ID= (int)UINT_MAX;

class Explain_query;

class Json_writer;
/* 
  A node can be either a SELECT, or a UNION.
*/
class Explain_node : public Sql_alloc
{
public:
  enum explain_node_type 
  {
    EXPLAIN_UNION, 
    EXPLAIN_SELECT, 
    EXPLAIN_UPDATE,
    EXPLAIN_DELETE, 
    EXPLAIN_INSERT
  };

  virtual enum explain_node_type get_type()= 0;
  virtual int get_select_id()= 0;

  /* 
    A node may have children nodes. When a node's explain structure is 
    created, children nodes may not yet have QPFs. This is why we store ids.
  */
  Dynamic_array<int> children;
  void add_child(int select_no)
  {
    children.append(select_no);
  }

  virtual int print_explain(Explain_query *query, select_result_sink *output, 
                            uint8 explain_flags, bool is_analyze)=0;
  virtual void print_explain_json(Explain_query *query, Json_writer *writer, 
                                  bool is_analyze)= 0;

  int print_explain_for_children(Explain_query *query, select_result_sink *output, 
                                 uint8 explain_flags, bool is_analyze);
  virtual ~Explain_node(){}
};


class Explain_table_access;


/*
  EXPLAIN structure for a SELECT.
  
  A select can be:
  1. A degenerate case. In this case, message!=NULL, and it contains a 
     description of what kind of degenerate case it is (e.g. "Impossible 
     WHERE").
  2. a non-degenrate join. In this case, join_tabs describes the join.

  In the non-degenerate case, a SELECT may have a GROUP BY/ORDER BY operation.

  In both cases, the select may have children nodes. class Explain_node provides
  a way get node's children.
*/

class Explain_select : public Explain_node
{
public:
  enum explain_node_type get_type() { return EXPLAIN_SELECT; }

  Explain_select() : 
    message(NULL), join_tabs(NULL),
    using_temporary(false), using_filesort(false)
  {}
  
  ~Explain_select();

  bool add_table(Explain_table_access *tab)
  {
    if (!join_tabs)
    {
      join_tabs= (Explain_table_access**) my_malloc(sizeof(Explain_table_access*) *
                                                MAX_TABLES, MYF(0));
      n_join_tabs= 0;
    }
    join_tabs[n_join_tabs++]= tab;
    return false;
  }
  
  /*
    This is used to save the results of "late" test_if_skip_sort_order() calls
    that are made from JOIN::exec
  */
  void replace_table(uint idx, Explain_table_access *new_tab);

public:
  int select_id;
  const char *select_type;

  int get_select_id() { return select_id; }

  /*
    If message != NULL, this is a degenerate join plan, and all subsequent
    members have no info 
  */
  const char *message;
  
  /*
    A flat array of Explain structs for tables. The order is "just like EXPLAIN
    would print them".
  */
  Explain_table_access** join_tabs;
  uint n_join_tabs;

  /* Global join attributes. In tabular form, they are printed on the first row */
  bool using_temporary;
  bool using_filesort;
  
  int print_explain(Explain_query *query, select_result_sink *output, 
                    uint8 explain_flags, bool is_analyze);
  void print_explain_json(Explain_query *query, Json_writer *writer, 
                          bool is_analyze);
  
  Table_access_tracker *get_using_temporary_read_tracker()
  {
    return &using_temporary_read_tracker;
  }
private:
  Table_access_tracker using_temporary_read_tracker;
};


/* 
  Explain structure for a UNION.

  A UNION may or may not have "Using filesort".
*/

class Explain_union : public Explain_node
{
public:
  enum explain_node_type get_type() { return EXPLAIN_UNION; }

  int get_select_id()
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
                          bool is_analyze);

  const char *fake_select_type;
  bool using_filesort;
  bool using_tmp;

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
  we do not intend for it survive until after query's MEM_ROOT is freed. It
  does surivive freeing of query's items.
   
  For reference, the process of post-query cleanup is as follows:

    >dispatch_command
    | >mysql_parse
    | |  ...
    | | lex_end()
    | |  ...
    | | >THD::cleanup_after_query
    | | | ...
    | | | free_items()
    | | | ...
    | | <THD::cleanup_after_query
    | |
    | <mysql_parse
    |
    | log_slow_statement()
    | 
    | free_root()
    | 
    >dispatch_command
  
  That is, the order of actions is:
    - free query's Items
    - write to slow query log 
    - free query's MEM_ROOT
    
*/

class Explain_query : public Sql_alloc
{
public:
  Explain_query(THD *thd);
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

  void print_explain_json(select_result_sink *output, bool is_analyze);

  /* If true, at least part of EXPLAIN can be printed */
  bool have_query_plan() { return insert_plan || upd_del_plan|| get_node(1) != NULL; }

  void query_plan_ready();

  MEM_ROOT *mem_root;

  Explain_update *get_upd_del_plan() { return upd_del_plan; }
private:
  /* Explain_delete inherits from Explain_update */
  Explain_update *upd_del_plan;

  /* Query "plan" for INSERTs */
  Explain_insert *insert_plan;

  Dynamic_array<Explain_union*> unions;
  Dynamic_array<Explain_select*> selects;
  
  THD *thd; // for APC start/stop
  bool apc_enabled;
  /* 
    Debugging aid: count how many times add_node() was called. Ideally, it
    should be one, we currently allow O(1) query plan saves for each
    select or union.  The goal is not to have O(#rows_in_some_table), which 
    is unacceptable.
  */
  longlong operations;
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

  ET_total
};


class EXPLAIN_BKA_TYPE
{
public:
  EXPLAIN_BKA_TYPE() : join_alg(NULL) {}

  bool incremental;
  const char *join_alg;
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
public:
  String_list key_parts_list;
  
  void clear()
  {
    key_name= NULL;
    key_len= (uint)-1;
  }
  void set(MEM_ROOT *root, KEY *key_name, uint key_len_arg);
  void set_pseudo_key(MEM_ROOT *root, const char *key_name);

  inline const char *get_key_name() { return key_name; }
  inline uint get_key_len() { return key_len; }
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
  EXPLAIN data structure for a single JOIN_TAB.
*/

class Explain_table_access : public Sql_alloc
{
public:
  void push_extra(enum explain_extra_tag extra_tag);

  /* Internals */
public:
  /* 
    0 means this tab is not inside SJM nest and should use Explain_select's id
    other value means the tab is inside an SJM nest.
  */
  int sjm_nest_select_id;

  /* id and 'select_type' are cared-of by the parent Explain_select */
  StringBuffer<32> table_name;

  enum join_type type;

  StringBuffer<32> used_partitions;
  bool used_partitions_set;
  
  /* Empty means "NULL" will be printed */
  String_list possible_keys;
  
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

  bool rows_set; /* not set means 'NULL' should be printed */
  ha_rows rows;

  bool filtered_set; /* not set means 'NULL' should be printed */
  double filtered;

  /* 
    Contents of the 'Extra' column. Some are converted into strings, some have
    parameters, values for which are stored below.
  */
  Dynamic_array<enum explain_extra_tag> extra_tags;

  // Valid if ET_USING tag is present
  Explain_quick_select *quick_info;

  // Valid if ET_USING_INDEX_FOR_GROUP_BY is present
  bool loose_scan_is_scanning;
  
  // valid with ET_RANGE_CHECKED_FOR_EACH_RECORD
  key_map range_checked_map;

  // valid with ET_USING_MRR
  StringBuffer<32> mrr_type;

  // valid with ET_USING_JOIN_BUFFER
  EXPLAIN_BKA_TYPE bka_type;
  
  StringBuffer<32> firstmatch_table_name;
  
  /*
    Note: lifespan of WHERE condition is less than lifespan of this object.
    THe below is valid if tags include "ET_USING_WHERE".
  */
  Item *where_cond;
  Item *pushed_index_cond;

  int print_explain(select_result_sink *output, uint8 explain_flags, 
                    bool is_analyze,
                    uint select_id, const char *select_type,
                    bool using_temporary, bool using_filesort);
  void print_explain_json(Json_writer *writer, bool is_analyze);

  /* ANALYZE members*/
  Table_access_tracker tracker;
  Table_access_tracker jbuf_tracker;

private:
  void append_tag_name(String *str, enum explain_extra_tag tag);
  void fill_key_str(String *key_str, bool is_json);
  void fill_key_len_str(String *key_len_str);
  double get_r_filtered();
  void tag_to_json(Json_writer *writer, enum explain_extra_tag tag);
};


/*
  EXPLAIN structure for single-table UPDATE. 
  
  This is similar to Explain_table_access, except that it is more restrictive.
  Also, it can have UPDATE operation options, but currently there aren't any.
*/

class Explain_update : public Explain_node
{
public:
  virtual enum explain_node_type get_type() { return EXPLAIN_UPDATE; }
  virtual int get_select_id() { return 1; /* always root */ }

  const char *select_type;

  StringBuffer<32> used_partitions;
  bool used_partitions_set;

  bool impossible_where;
  bool no_partitions;
  StringBuffer<64> table_name;

  enum join_type jtype;
  StringBuffer<128> possible_keys_line;
  StringBuffer<128> key_str;
  StringBuffer<128> key_len_str;
  StringBuffer<64> mrr_type;
  
  Explain_quick_select *quick_info;

  bool using_where;
  ha_rows rows;

  bool using_filesort;
  bool using_io_buffer;

  /* ANALYZE members and methods */
  Table_access_tracker tracker;

  virtual int print_explain(Explain_query *query, select_result_sink *output, 
                            uint8 explain_flags, bool is_analyze);
  virtual void print_explain_json(Explain_query *query, Json_writer *writer, bool is_analyze)
  { /* EXPLAIN_JSON_NOT_IMPL */}
};


/*
  EXPLAIN data structure for an INSERT.
  
  At the moment this doesn't do much as we don't really have any query plans
  for INSERT statements.
*/

class Explain_insert : public Explain_node
{
public:
  StringBuffer<64> table_name;

  enum explain_node_type get_type() { return EXPLAIN_INSERT; }
  int get_select_id() { return 1; /* always root */ }

  int print_explain(Explain_query *query, select_result_sink *output, 
                    uint8 explain_flags, bool is_analyze);
  void print_explain_json(Explain_query *query, Json_writer *writer, 
                          bool is_analyze)
  { /* EXPLAIN_JSON_NOT_IMPL */}
};


/* 
  EXPLAIN data of a single-table DELETE.
*/

class Explain_delete: public Explain_update
{
public:
  /*
    TRUE means we're going to call handler->delete_all_rows() and not read any
    rows.
  */
  bool deleting_all_rows;

  virtual enum explain_node_type get_type() { return EXPLAIN_DELETE; }
  virtual int get_select_id() { return 1; /* always root */ }

  virtual int print_explain(Explain_query *query, select_result_sink *output, 
                            uint8 explain_flags, bool is_analyze);
  virtual void print_explain_json(Explain_query *query, Json_writer *writer, bool is_analyze)
  { /* EXPLAIN_JSON_NOT_IMPL */}
};


