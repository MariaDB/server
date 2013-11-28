
/*
  This file is a "bridge" interface between cassandra+Thrift and MariaDB.

  It is #included by both sides, so it must itself include neither (including
  both together causes compile errors due to conflicts).
*/

struct st_mysql_lex_string;
typedef struct st_mysql_lex_string LEX_STRING;

/* We need to define this here so that ha_cassandra.cc also has access to it */
typedef enum
{
  ONE = 1-1,
  QUORUM = 2-1,
  LOCAL_QUORUM = 3-1,
  EACH_QUORUM = 4-1,
  ALL = 5-1,
  ANY = 6-1,
  TWO = 7-1,
  THREE = 8-1,
} enum_cassandra_consistency_level;


class Column_name_enumerator
{
public:
  virtual const char* get_next_name()=0;
  virtual ~Column_name_enumerator(){}
};

/*
  Interface to one cassandra column family, i.e. one 'table'
*/
class Cassandra_se_interface
{
public:
  Cassandra_se_interface() { err_buffer[0]=0; }

  virtual ~Cassandra_se_interface(){};
  /* Init */
  virtual bool connect(const char *host, int port, const char *keyspace)=0;
  virtual void set_column_family(const char *cfname) = 0;

  /* Settings */
  virtual void set_consistency_levels(unsigned long read_cons_level, unsigned long write_cons_level)=0;
  virtual void set_n_retries(uint retries_arg)=0;

  /* Check underlying DDL */
  virtual bool setup_ddl_checks()=0;
  virtual void first_ddl_column()=0;
  virtual bool next_ddl_column(char **name, int *name_len, char **value,
                               int *value_len)=0;
  virtual void get_rowkey_type(char **name, char **type)=0;
  virtual size_t get_ddl_size()=0;
  virtual const char* get_default_validator()=0;

  /* Writes */
  virtual void clear_insert_buffer()=0;
  virtual void add_row_deletion(const char *key, int key_len,
                                Column_name_enumerator *col_names,
                                LEX_STRING *names, uint nnames)=0;
  virtual void start_row_insert(const char *key, int key_len)=0;
  virtual void add_insert_delete_column(const char *name, int name_len)= 0;
  virtual void add_insert_column(const char *name, int name_len,
                                 const char *value,
                                 int value_len)=0;
  virtual bool do_insert()=0;

  /* Reads */
  virtual bool get_slice(char *key, size_t key_len, bool *found)=0 ;
  virtual bool get_next_read_column(char **name, int *name_len,
                                    char **value, int *value_len)=0;
  virtual void get_read_rowkey(char **value, int *value_len)=0;

  /* Reads, multi-row scans */
  int read_batch_size;
  virtual bool get_range_slices(bool last_key_as_start_key)=0;
  virtual void finish_reading_range_slices()=0;
  virtual bool get_next_range_slice_row(bool *eof)=0;

  /* Reads, MRR scans */
  virtual void new_lookup_keys()=0;
  virtual int  add_lookup_key(const char *key, size_t key_len)=0;
  virtual bool multiget_slice()=0;
  virtual bool get_next_multiget_row()=0;

  /* read_set setup */
  virtual void clear_read_columns()=0;
  virtual void clear_read_all_columns()=0;
  virtual void add_read_column(const char *name)=0;

  virtual bool truncate()=0;
  virtual bool remove_row()=0;

  /* Passing error messages up to ha_cassandra */
  char err_buffer[512];
  const char *error_str() { return err_buffer; }
  void print_error(const char *format, ...);
};


/* A structure with global counters */
class Cassandra_status_vars
{
public:
  unsigned long row_inserts;
  unsigned long row_insert_batches;

  unsigned long multiget_reads;
  unsigned long multiget_keys_scanned;
  unsigned long multiget_rows_read;

  unsigned long timeout_exceptions;
  unsigned long unavailable_exceptions;
  unsigned long network_exceptions;
};


extern Cassandra_status_vars cassandra_counters;


Cassandra_se_interface *create_cassandra_se();

