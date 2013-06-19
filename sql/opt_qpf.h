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


  virtual int get_select_id()= 0;

  /* 
    A node may have children nodes. When a node's QPF (Query Plan Footprint) is
    created, children nodes may not yet have QPFs.  This is why we store ids.
  */
  Dynamic_array<int> children;
  void add_child(int select_no)
  {
    children.append(select_no);
  }

  virtual int print_explain(QPF_query *query, select_result_sink *output, 
                            uint8 explain_flags)=0;

  virtual ~QPF_node(){}
};


class QPF_table_access;


/*
  Query Plan Footprint of a SELECT.
  
  A select can be:
  - a degenerate case. In this case, message!=NULL, and it contains a 
    description of what kind of degenerate case it is (e.g. "Impossible 
    WHERE").
  - a join. Here join_tabs has an array of JOIN_TAB query plan footprints.

  In the non-degenerate case, a SELECT may have a GROUP BY/ORDER BY operation.
  In both cases, a select may have children selects (see QPF_node)
*/

class QPF_select : public QPF_node
{
  /*Construction interface */ 
public:
  enum qpf_node_type get_type() { return QPF_SELECT; }

  QPF_select() : 
    message(NULL), join_tabs(NULL),
    using_temporary(false), using_filesort(false)
  {}
  
  ~QPF_select();

  bool add_table(QPF_table_access *tab)
  {
    if (!join_tabs)
    {
      join_tabs= (QPF_table_access**) my_malloc(sizeof(QPF_table_access*) *
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
    A flat array of Query Plan Footprints. The order is "just like EXPLAIN
    would print them".
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


/* 
  Query Plan Footprint of a UNION.

  A UNION may or may not have "Using filesort".
*/

class QPF_union : public QPF_node
{
public:
  enum qpf_node_type get_type() { return QPF_UNION; }

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
  void push_table_name(List<Item> *item_list);
  int print_explain(QPF_query *query, select_result_sink *output, 
                    uint8 explain_flags);

  const char *fake_select_type;
  bool using_filesort;
};


/*
  Query Plan Footprint for a query (i.e. a statement)
*/

class QPF_query : public Sql_alloc
{
public:
  QPF_query();
  ~QPF_query();
  /* Add a new node */
  void add_node(QPF_node *node);

  /* This will return a select, or a union */
  QPF_node *get_node(uint select_id);

  /* This will return a select (even if there is a union with this id) */
  QPF_select *get_select(uint select_id);
  
  /* Delete_plan inherits from Update_plan */
  Update_plan *upd_del_plan;

  /* Produce a tabular EXPLAIN output */
  int print_explain(select_result_sink *output, uint8 explain_flags);
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


/*
  Query Plan Footprint for a JOIN_TAB.
*/
class QPF_table_access : public Sql_alloc
{
public:
  void push_extra(enum Extra_tag extra_tag);

  /* Internals */
public:
  /* 
    0 means this tab is not inside SJM nest and should use QPF_select's id
    other value means the tab is inside an SJM nest.
  */
  int sjm_nest_select_id;

  /* id and 'select_type' are cared-of by the parent QPF_select */
  TABLE *table;
  StringBuffer<64> table_name;

  enum join_type type;

  StringBuffer<64> used_partitions;
  bool used_partitions_set;

  key_map possible_keys;
  StringBuffer<64> possible_keys_str;
  
  /* Not used? */
  uint key_no;
  uint key_length;

  Dynamic_array<enum Extra_tag> extra_tags;

  //temporary:
  bool key_set; /* not set means 'NULL' should be printed */
  StringBuffer<64> key;

  bool key_len_set; /* not set means 'NULL' should be printed */
  StringBuffer<64> key_len;

  bool ref_set; /* not set means 'NULL' should be printed */
  StringBuffer<64> ref;

  bool rows_set;
  ha_rows rows;

  bool filtered_set;
  double filtered;

  /* Various stuff for 'Extra' column*/
  uint join_cache_level;
  
  // Valid if ET_USING tag is present
  StringBuffer<64> quick_info;

  // Valid if ET_USING_INDEX_FOR_GROUP_BY is present
  StringBuffer<64> loose_scan_type;
  
  // valid with ET_RANGE_CHECKED_FOR_EACH_RECORD
  key_map range_checked_map;

  // valid with ET_USING_MRR
  StringBuffer<64> mrr_type;

  // valid with ET_USING_JOIN_BUFFER
  StringBuffer<64> join_buffer_type;
  
  //TABLE *firstmatch_table;
  StringBuffer<64> firstmatch_table_name;

  int print_explain(select_result_sink *output, uint8 explain_flags, 
                    uint select_id, const char *select_type,
                    bool using_temporary, bool using_filesort);
private:
  void append_tag_name(String *str, enum Extra_tag tag);
};

// TODO: should Update_plan inherit from QPF_table_access?

