struct st_spider_conn;
typedef st_spider_conn SPIDER_CONN;

typedef struct spider_table_link_idx_holder SPIDER_TABLE_LINK_IDX_HOLDER;
typedef struct spider_table_holder SPIDER_TABLE_HOLDER;

class ha_spider;
class spider_string;

typedef struct spider_link_idx_holder
{
  spider_table_link_idx_holder *table_link_idx_holder;
  int link_idx;
  int link_status;
  spider_link_idx_holder *next_table;
  spider_link_idx_holder *next;
} SPIDER_LINK_IDX_HOLDER;

typedef struct spider_link_idx_chain
{
  SPIDER_CONN *conn;
  spider_link_idx_holder *link_idx_holder;
  spider_link_idx_holder *current_link_idx_holder;
  int link_status;
  spider_link_idx_chain *next;
} SPIDER_LINK_IDX_CHAIN;

typedef struct spider_table_link_idx_holder
{
  spider_table_holder *table_holder;
  spider_link_idx_holder *first_link_idx_holder;
  spider_link_idx_holder *last_link_idx_holder;
  spider_link_idx_holder *current_link_idx_holder;
  uint link_idx_holder_count;
} SPIDER_TABLE_LINK_IDX_HOLDER;

typedef struct spider_conn_holder
{
  SPIDER_CONN *conn;
  spider_table_link_idx_holder *table_link_idx_holder;
  uint link_idx_holder_count_max;
  bool checked_for_same_conn;
  long access_balance;
  spider_conn_holder *prev;
  spider_conn_holder *next;
} SPIDER_CONN_HOLDER;

/* Record information of a local (spider) table, for use of the spider
group by handler. */
typedef struct spider_table_holder
{
  TABLE *table;
  ha_spider *spider;
  /* alias of the table, in the form of tk, where k is the index of
  the table from `query->from' indexed by next_local. */
  spider_string *alias;
} SPIDER_TABLE_HOLDER;

/* For use of the spider group by handler. */
class spider_fields
{
  uint dbton_count;
  uint current_dbton_num;
  uint dbton_ids[SPIDER_DBTON_SIZE];
  /* Number of tables in `query->from'. */
  uint table_count;
  /* All tables in `query->from', in the same order by next_local. */
  SPIDER_TABLE_HOLDER *table_holder;
  SPIDER_LINK_IDX_CHAIN *first_link_idx_chain;
  SPIDER_LINK_IDX_CHAIN *last_link_idx_chain;
  SPIDER_LINK_IDX_CHAIN *current_link_idx_chain;
  SPIDER_LINK_IDX_CHAIN *first_ok_link_idx_chain;
  SPIDER_CONN_HOLDER *first_conn_holder;
  SPIDER_CONN_HOLDER *last_conn_holder;
  SPIDER_CONN_HOLDER *current_conn_holder;
  Field **current_field_ptr;
public:
  spider_fields();
  virtual ~spider_fields();
  void add_dbton_id(
    uint dbton_id_arg
  );
  void set_pos_to_first_dbton_id();
  uint get_next_dbton_id();
  int make_link_idx_chain(
    int link_status
  );
  SPIDER_LINK_IDX_CHAIN *create_link_idx_chain();
  void set_pos_to_first_link_idx_chain();
  SPIDER_LINK_IDX_CHAIN *get_next_link_idx_chain();
  SPIDER_LINK_IDX_HOLDER *get_dup_link_idx_holder(
    SPIDER_TABLE_LINK_IDX_HOLDER *table_link_idx_holder,
    SPIDER_LINK_IDX_HOLDER *current
  );
  bool check_link_ok_chain();
  bool is_first_link_ok_chain(
    SPIDER_LINK_IDX_CHAIN *link_idx_chain_arg
  );
  int get_ok_link_idx();
  void set_first_link_idx();
  int add_link_idx(
    SPIDER_CONN_HOLDER *conn_holder_arg,
    ha_spider *spider_arg,
    int link_idx
  );
  SPIDER_LINK_IDX_HOLDER *create_link_idx_holder();
  void set_pos_to_first_table_on_link_idx_chain(
    SPIDER_LINK_IDX_CHAIN *link_idx_chain_arg
  );
  SPIDER_LINK_IDX_HOLDER *get_next_table_on_link_idx_chain(
    SPIDER_LINK_IDX_CHAIN *link_idx_chain_arg
  );
  SPIDER_CONN_HOLDER *add_conn(
    SPIDER_CONN *conn_arg,
    long access_balance
  );
  SPIDER_CONN_HOLDER *create_conn_holder();
  bool has_conn_holder();
  void clear_conn_holder_checked();
  bool check_conn_same_conn(
    SPIDER_CONN *conn_arg
  );
  bool remove_conn_if_not_checked();
  void check_support_dbton(
    uchar *dbton_bitmap
  );
  void choose_a_conn();
  void free_conn_holder(
    SPIDER_CONN_HOLDER *conn_holder_arg
  );
  SPIDER_TABLE_HOLDER *find_table(Field *field);
  void set_table_holder(
    SPIDER_TABLE_HOLDER *table_holder_arg,
    uint table_count_arg
  );
  SPIDER_TABLE_HOLDER *get_first_table_holder();
  SPIDER_TABLE_HOLDER *get_table_holder(TABLE *table);
  uint get_table_count();
  void set_field_ptr(Field **field_arg);
  Field **get_next_field_ptr();
  int ping_table_mon_from_table(
    SPIDER_LINK_IDX_CHAIN *link_idx_chain
  );
};

SPIDER_TABLE_HOLDER *spider_create_table_holder(
  uint table_count_arg
);

SPIDER_TABLE_HOLDER *spider_add_table_holder(
  ha_spider *spider_arg,
  SPIDER_TABLE_HOLDER *table_holder
);

int spider_make_query(const Query& query, spider_fields* fields,
                      ha_spider *spider, TABLE *table);
