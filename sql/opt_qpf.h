/**************************************************************************************
 
  Query Plan Footprint (QPF) structures

  These structures
  - Can be produced in-expensively from query plan.
  - Store sufficient information to produce either a tabular or a json EXPLAIN
    output
  - Have methods that produce a tabular output.
 
*************************************************************************************/

class QPF_query;

/* 
  A node can be either a SELECT, or a UNION.
*/
class QPF_node : public Sql_alloc
{
public:
  enum qpf_node_type {QPF_UNION, QPF_SELECT};

  virtual enum qpf_node_type get_type()= 0;
  virtual int print_explain(QPF_query *query, select_result_sink *output, 
                            uint8 explain_flags)=0;
  virtual ~QPF_node(){}
};


/*
  Nesting. 
    QPF_select may have children QPF_select-s. 
      (these can be FROM-subqueries, or subqueries from other clauses)

  As for unions, the standard approach is:
   - UNION node can be where the select node can be;
   - the union has a select that retrieves results from temptable (a special
     kind of child)
   - and it has regular children selects that are merged into the union.

*/

class QPF_table_access;

class QPF_select : public QPF_node
{
  /*Construction interface */ 
public:
  enum qpf_node_type get_type() { return QPF_SELECT; }

#if 0  
  /* Constructs a finished degenerate join plan */
  QPF_select(int select_id_arg, const char *select_type_arg, const char* msg) : 
    select_id(select_id_arg),
    select_type(select_type_arg),
    message(msg), 
    join_tabs(NULL), n_join_tabs(0) 
  {}
  
  /* Constructs an un-finished, non degenerate join plan. */
  QPF_select(int select_id_arg, const char *select_type_arg) : 
    select_id(select_id_arg),
    select_type(select_type_arg),
    message(NULL), 
    join_tabs(NULL), n_join_tabs(0) 
  {}
#endif
  QPF_select() : 
    message(NULL), join_tabs(NULL),
    using_temporary(false), using_filesort(false)
  {}

  bool add_table(QPF_table_access *tab)
  {
    if (!join_tabs)
    {
      join_tabs= (QPF_table_access**) malloc(sizeof(QPF_table_access*) * MAX_TABLES);
      n_join_tabs= 0;
    }
    join_tabs[n_join_tabs++]= tab;
    return false;
  }

public:
  int select_id; /* -1 means NULL. */
  const char *select_type;

  /*
    If message != NULL, this is a degenerate join plan, and all subsequent
    members have no info 
  */
  const char *message;
  
  /*
    According to the discussion: this should be an array of "table
    descriptors".

    As for SJ-Materialization. Start_materialize/end_materialize markers?
  */
  QPF_table_access** join_tabs;
  uint n_join_tabs;

  /* Global join attributes. In tabular form, they are printed on the first row */
  bool using_temporary;
  bool using_filesort;

  void print_tabular(select_result_sink *output, uint8 explain_flags//, 
                     //bool *printed_anything
                    );

  int print_explain(QPF_query *query, select_result_sink *output, 
                    uint8 explain_flags);
};


class QPF_union : public QPF_node
{
public:
  enum qpf_node_type get_type() { return QPF_UNION; }

  int get_select_id()
  {
    DBUG_ASSERT(children.elements() > 0);
    return children.at(0);
  }
  // This has QPF_select children
  Dynamic_array<int> children;

  void add_select(int select_no)
  {
    children.append(select_no);
  }
  void push_table_name(List<Item> *item_list);
  int print_explain(QPF_query *query, select_result_sink *output, 
                    uint8 explain_flags);

  const char *fake_select_type;
  bool using_filesort;
};


/*
  This is the whole query. 
*/

class QPF_query 
{
public:
  QPF_query();
  void add_node(QPF_node *node);
  int print_explain(select_result_sink *output, uint8 explain_flags);

  /* This will return a select, or a union */
  QPF_node *get_node(uint select_id);

  /* This will return a select (even if there is a union with this id) */
  QPF_select *get_select(uint select_id);

private:
  QPF_union *unions[MAX_TABLES];
  QPF_select *selects[MAX_TABLES];
};


enum Extra_tag
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


class QPF_table_access
{
public:
  void push_extra(enum Extra_tag extra_tag);

  /* Internals */
public:
  /* id and 'select_type' are cared-of by the parent QPF_select */
  TABLE *table;
  StringBuffer<256> table_name;

  enum join_type type;

  StringBuffer<256> used_partitions;
  bool used_partitions_set;

  key_map possible_keys;
  
  uint key_no;
  uint key_length;

  Dynamic_array<enum Extra_tag> extra_tags;

  //temporary:
  StringBuffer<256> key;
  StringBuffer<256> key_len;
  StringBuffer<256> ref;
  bool key_set;
  bool key_len_set;
  bool ref_set;

  bool rows_set;
  ha_rows rows;

  double filtered;
  bool filtered_set;

  /* Various stuff for 'Extra' column*/
  uint join_cache_level;
  
  // Valid if ET_USING tag is present
  StringBuffer<256> quick_info;

  // Valid if ET_USING_INDEX_FOR_GROUP_BY is present
  StringBuffer<256> loose_scan_type;
  
  // valid with ET_RANGE_CHECKED_FOR_EACH_RECORD
  key_map range_checked_map;

  // valid with ET_USING_MRR
  StringBuffer <256> mrr_type;

  // valid with ET_USING_JOIN_BUFFER
  StringBuffer <256> join_buffer_type;
  
  TABLE *firstmatch_table;

  int print_explain(select_result_sink *output, uint8 explain_flags, 
                    uint select_id, const char *select_type,
                    bool using_temporary, bool using_filesort);
private:
  void append_tag_name(String *str, enum Extra_tag tag);
};

// Update_plan and Delete_plan belong to this kind of structures, too.

// TODO: should Update_plan inherit from QPF_table_access?


