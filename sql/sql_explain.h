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


/**************************************************************************************
 
  Data structures for producing EXPLAIN outputs.

  These structures
  - Can be produced inexpensively from query plan.
  - Store sufficient information to produce tabular EXPLAIN output (the goal is 
    to be able to produce JSON also)

*************************************************************************************/


const int FAKE_SELECT_LEX_ID= (int)UINT_MAX;

class Explain_query;

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
                            uint8 explain_flags)=0;
  
  int print_explain_for_children(Explain_query *query, select_result_sink *output, 
                                 uint8 explain_flags);
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
                    uint8 explain_flags);
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
                    uint8 explain_flags);

  const char *fake_select_type;
  bool using_filesort;
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
  int print_explain(select_result_sink *output, uint8 explain_flags);
  
  /* Send tabular EXPLAIN to the client */
  int send_explain(THD *thd);
  
  /* Return tabular EXPLAIN output as a text string */
  bool print_explain_str(THD *thd, String *out_str);

  /* If true, at least part of EXPLAIN can be printed */
  bool have_query_plan() { return insert_plan || upd_del_plan|| get_node(1) != NULL; }

  void query_plan_ready();

  MEM_ROOT *mem_root;
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


typedef struct st_explain_bka_type
{
  bool incremental;
  const char *join_alg;
  StringBuffer<64> mrr_type;

} EXPLAIN_BKA_TYPE;


/*
  Data about how an index is used by some access method
*/
class Explain_index_use : public Sql_alloc
{
  char *key_name;
  uint key_len;
  /* will add #keyparts here if we implement EXPLAIN FORMAT=JSON */
public:

  void set(MEM_ROOT *root, const char *key_name_arg, uint key_len_arg)
  {
    if (key_name_arg)
    {
      size_t name_len= strlen(key_name_arg);
      if ((key_name= (char*)alloc_root(root, name_len+1)))
        memcpy(key_name, key_name_arg, name_len+1);
    }
    else
      key_name= NULL;
    key_len= key_len_arg;
  }

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
  
  /* This is used when quick_type == QUICK_SELECT_I::QS_TYPE_RANGE */
  Explain_index_use range;
  
  /* Used in all other cases */
  List<Explain_quick_select> children;
  
  void print_extra(String *str);
  void print_key(String *str);
  void print_key_len(String *str);
private:
  void print_extra_recursive(String *str);
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
  
  /* Empty string means "NULL" will be printed */
  StringBuffer<32> possible_keys_str;
  
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
  
  bool ref_set; /* not set means 'NULL' should be printed */
  StringBuffer<32> ref;

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

  int print_explain(select_result_sink *output, uint8 explain_flags, 
                    uint select_id, const char *select_type,
                    bool using_temporary, bool using_filesort);
private:
  void append_tag_name(String *str, enum explain_extra_tag tag);
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

  virtual int print_explain(Explain_query *query, select_result_sink *output, 
                            uint8 explain_flags);
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
                    uint8 explain_flags);
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
                            uint8 explain_flags);
};


