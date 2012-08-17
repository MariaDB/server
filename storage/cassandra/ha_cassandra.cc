/* 
  MP AB copyrights 
*/

#ifdef USE_PRAGMA_IMPLEMENTATION
#pragma implementation        // gcc: Class implementation
#endif

#include <mysql/plugin.h>
#include "ha_cassandra.h"
#include "sql_class.h"

static handler *cassandra_create_handler(handlerton *hton,
                                       TABLE_SHARE *table, 
                                       MEM_ROOT *mem_root);


handlerton *cassandra_hton;


/* 
   Hash used to track the number of open tables; variable for example share
   methods
*/
static HASH cassandra_open_tables;

/* The mutex used to init the hash; variable for example share methods */
mysql_mutex_t cassandra_mutex;


/**
  Structure for CREATE TABLE options (table options).
  It needs to be called ha_table_option_struct.

  The option values can be specified in the CREATE TABLE at the end:
  CREATE TABLE ( ... ) *here*
*/

struct ha_table_option_struct
{
  const char *host;
  const char *keyspace;
  const char *column_family;
};


ha_create_table_option cassandra_table_option_list[]=
{
  /*
    one option that takes an arbitrary string
  */
  HA_TOPTION_STRING("thrift_host", host),
  HA_TOPTION_STRING("keyspace", keyspace),
  HA_TOPTION_STRING("column_family", column_family),
  HA_TOPTION_END
};


/**
  @brief
  Function we use in the creation of our hash to get key.
*/

static uchar* cassandra_get_key(CASSANDRA_SHARE *share, size_t *length,
                             my_bool not_used __attribute__((unused)))
{
  *length=share->table_name_length;
  return (uchar*) share->table_name;
}

#ifdef HAVE_PSI_INTERFACE
static PSI_mutex_key ex_key_mutex_example, ex_key_mutex_CASSANDRA_SHARE_mutex;

static PSI_mutex_info all_cassandra_mutexes[]=
{
  { &ex_key_mutex_example, "cassandra", PSI_FLAG_GLOBAL},
  { &ex_key_mutex_CASSANDRA_SHARE_mutex, "CASSANDRA_SHARE::mutex", 0}
};

static void init_cassandra_psi_keys()
{
  const char* category= "cassandra";
  int count;

  if (PSI_server == NULL)
    return;

  count= array_elements(all_cassandra_mutexes);
  PSI_server->register_mutex(category, all_cassandra_mutexes, count);
}
#endif

static int cassandra_init_func(void *p)
{
  DBUG_ENTER("cassandra_init_func");

#ifdef HAVE_PSI_INTERFACE
  init_cassandra_psi_keys();
#endif

  cassandra_hton= (handlerton *)p;
  mysql_mutex_init(ex_key_mutex_example, &cassandra_mutex, MY_MUTEX_INIT_FAST);
  (void) my_hash_init(&cassandra_open_tables,system_charset_info,32,0,0,
                      (my_hash_get_key) cassandra_get_key,0,0);

  cassandra_hton->state=   SHOW_OPTION_YES;
  cassandra_hton->create=  cassandra_create_handler;
  cassandra_hton->flags=   HTON_CAN_RECREATE;
  cassandra_hton->table_options= cassandra_table_option_list;
  //cassandra_hton->field_options= example_field_option_list;
  cassandra_hton->field_options= NULL;

  DBUG_RETURN(0);
}


static int cassandra_done_func(void *p)
{
  int error= 0;
  DBUG_ENTER("cassandra_done_func");
  if (cassandra_open_tables.records)
    error= 1;
  my_hash_free(&cassandra_open_tables);
  mysql_mutex_destroy(&cassandra_mutex);
  DBUG_RETURN(error);
}


/**
  @brief
  Example of simple lock controls. The "share" it creates is a
  structure we will pass to each cassandra handler. Do you have to have
  one of these? Well, you have pieces that are used for locking, and
  they are needed to function.
*/

static CASSANDRA_SHARE *get_share(const char *table_name, TABLE *table)
{
  CASSANDRA_SHARE *share;
  uint length;
  char *tmp_name;

  mysql_mutex_lock(&cassandra_mutex);
  length=(uint) strlen(table_name);

  if (!(share=(CASSANDRA_SHARE*) my_hash_search(&cassandra_open_tables,
                                              (uchar*) table_name,
                                              length)))
  {
    if (!(share=(CASSANDRA_SHARE *)
          my_multi_malloc(MYF(MY_WME | MY_ZEROFILL),
                          &share, sizeof(*share),
                          &tmp_name, length+1,
                          NullS)))
    {
      mysql_mutex_unlock(&cassandra_mutex);
      return NULL;
    }

    share->use_count=0;
    share->table_name_length=length;
    share->table_name=tmp_name;
    strmov(share->table_name,table_name);
    if (my_hash_insert(&cassandra_open_tables, (uchar*) share))
      goto error;
    thr_lock_init(&share->lock);
    mysql_mutex_init(ex_key_mutex_CASSANDRA_SHARE_mutex,
                     &share->mutex, MY_MUTEX_INIT_FAST);
  }
  share->use_count++;
  mysql_mutex_unlock(&cassandra_mutex);

  return share;

error:
  mysql_mutex_destroy(&share->mutex);
  my_free(share);

  return NULL;
}


/**
  @brief
  Free lock controls. We call this whenever we close a table. If the table had
  the last reference to the share, then we free memory associated with it.
*/

static int free_share(CASSANDRA_SHARE *share)
{
  mysql_mutex_lock(&cassandra_mutex);
  if (!--share->use_count)
  {
    my_hash_delete(&cassandra_open_tables, (uchar*) share);
    thr_lock_delete(&share->lock);
    mysql_mutex_destroy(&share->mutex);
    my_free(share);
  }
  mysql_mutex_unlock(&cassandra_mutex);

  return 0;
}


static handler* cassandra_create_handler(handlerton *hton,
                                       TABLE_SHARE *table, 
                                       MEM_ROOT *mem_root)
{
  return new (mem_root) ha_cassandra(hton, table);
}


ha_cassandra::ha_cassandra(handlerton *hton, TABLE_SHARE *table_arg)
  :handler(hton, table_arg),
   se(NULL), names_and_vals(NULL)
{}


static const char *ha_cassandra_exts[] = {
  NullS
};

const char **ha_cassandra::bas_ext() const
{
  return ha_cassandra_exts;
}


int ha_cassandra::open(const char *name, int mode, uint test_if_locked)
{
  DBUG_ENTER("ha_cassandra::open");

  if (!(share = get_share(name, table)))
    DBUG_RETURN(1);
  thr_lock_data_init(&share->lock,&lock,NULL);
  
  ha_table_option_struct *options= table->s->option_struct;
  fprintf(stderr, "ha_cass: open thrift_host=%s keyspace=%s column_family=%s\n", 
          options->host, options->keyspace, options->column_family);
  
  DBUG_ASSERT(!se);
  if (!options->host || !options->keyspace || !options->column_family)
    DBUG_RETURN(HA_WRONG_CREATE_OPTION);
  se= get_cassandra_se();
  se->set_column_family(options->column_family);
  if (se->connect(options->host, options->keyspace))
  {
    my_error(ER_CONNECT_TO_FOREIGN_DATA_SOURCE, MYF(0), se->error_str());
    DBUG_RETURN(HA_ERR_NO_CONNECTION);
  }

  DBUG_RETURN(0);
}


int ha_cassandra::close(void)
{
  DBUG_ENTER("ha_cassandra::close");
  delete se;
  se= NULL;
  if (names_and_vals)
  {
    my_free(names_and_vals);
    names_and_vals= NULL;
  }
  DBUG_RETURN(free_share(share));
}


/**
  @brief
  create() is called to create a database. The variable name will have the name
  of the table.

  @details
  When create() is called you do not need to worry about
  opening the table. Also, the .frm file will have already been
  created so adjusting create_info is not necessary. You can overwrite
  the .frm file at this point if you wish to change the table
  definition, but there are no methods currently provided for doing
  so.

  Called from handle.cc by ha_create_table().

  @see
  ha_create_table() in handle.cc
*/

int ha_cassandra::create(const char *name, TABLE *table_arg,
                         HA_CREATE_INFO *create_info)
{
  ha_table_option_struct *options= table_arg->s->option_struct;
  DBUG_ENTER("ha_cassandra::create");
  DBUG_ASSERT(options);
  //psergey-todo: This is called for CREATE TABLE... check options here.
  
/*
  if (table_arg->s->fields != 2)
  {
    my_error(ER_WRONG_COLUMN_NAME, MYF(0), "The table must have two fields");
    DBUG_RETURN(HA_WRONG_CREATE_OPTION);
  }
*/

  Field **pfield= table_arg->s->field;
  if (strcmp((*pfield)->field_name, "rowkey"))
  {
    my_error(ER_WRONG_COLUMN_NAME, MYF(0), "First column must be named 'rowkey'");
    DBUG_RETURN(HA_WRONG_CREATE_OPTION);
  }

  if (table_arg->s->keys != 1 || table_arg->s->primary_key !=0 ||
      table_arg->key_info[0].key_parts != 1 ||
      table_arg->key_info[0].key_part[0].fieldnr != 1)
  {
    my_error(ER_WRONG_COLUMN_NAME, MYF(0), "Table must have one PRIMARY KEY(rowkey)");
    DBUG_RETURN(HA_WRONG_CREATE_OPTION);
  }

/*
  pfield++;
  if (strcmp((*pfield)->field_name, "data"))
  {
    my_error(ER_WRONG_COLUMN_NAME, MYF(0), "Second column must be named 'data'");
    DBUG_RETURN(HA_WRONG_CREATE_OPTION);
  }
*/
  


#ifndef DBUG_OFF
/*  
  DBUG_PRINT("info", ("strparam: '%-.64s'  ullparam: %llu  enumparam: %u  "\
                      "boolparam: %u",
                      (options->strparam ? options->strparam : "<NULL>"),
                      options->ullparam, options->enumparam, options->boolparam));

  psergey-todo: check table definition!
  for (Field **field= table_arg->s->field; *field; field++)
  {
    ha_field_option_struct *field_options= (*field)->option_struct;
    DBUG_ASSERT(field_options);
    DBUG_PRINT("info", ("field: %s  complex: '%-.64s'",
                         (*field)->field_name,
                         (field_options->complex_param_to_parse_it_in_engine ?
                          field_options->complex_param_to_parse_it_in_engine :
                          "<NULL>")));
  }
*/
#endif
  DBUG_ASSERT(!se);
  if (!options->host || !options->keyspace || !options->column_family)
    DBUG_RETURN(HA_WRONG_CREATE_OPTION);
  se= get_cassandra_se();
  se->set_column_family(options->column_family);
  if (se->connect(options->host, options->keyspace))
  {
    my_error(ER_CONNECT_TO_FOREIGN_DATA_SOURCE, MYF(0), se->error_str());
    DBUG_RETURN(HA_ERR_NO_CONNECTION);
  }

  /* 
    TODO: what about mapping the primary key? It has a 'type', too...
    see CfDef::key_validation_class ? see also CfDef::key_alias?
  */
  se->first_ddl_column();
  char *col_name;
  int  col_name_len;
  char *col_type;
  int col_type_len;
  while (!se->next_ddl_column(&col_name, &col_name_len, &col_type,
                              &col_type_len))
  {
    /* Mapping for the 1st field is already known */
    for (Field **field= table_arg->s->field + 1; *field; field++)
    {
      if (!strcmp((*field)->field_name, col_name))
      {
        //map_field_to_type(field, col_type);
      }
    }
  }

  DBUG_RETURN(0);
}

/*
  Mapping needs to
  - copy value from MySQL record to Thrift buffer
  - copy value from Thrift bufer to MySQL record..

*/

const char * const validator_bigint="org.apache.cassandra.db.marshal.LongType";
const char * const validator_int="org.apache.cassandra.db.marshal.Int32Type";
const char * const validator_counter= "org.apache.cassandra.db.marshal.CounterColumnType";

const char * const validator_float= "org.apache.cassandra.db.marshal.FloatType";
const char * const validator_double= "org.apache.cassandra.db.marshal.DoubleType";

void map_field_to_type(Field *field, const char *validator_name)
{
  switch(field->type()) {
    case MYSQL_TYPE_TINY:
    case MYSQL_TYPE_SHORT:
    case MYSQL_TYPE_LONG:
    case MYSQL_TYPE_LONGLONG:
      if (!strcmp(validator_name, validator_bigint))
      {
        //setup bigint validator
      }
      break;
    case MYSQL_TYPE_FLOAT:
      if (!strcmp(validator_name, validator_float))
      break;
    case MYSQL_TYPE_DOUBLE:
      if (!strcmp(validator_name, validator_double))
      break;
    default:
      DBUG_ASSERT(0);
  }
}


void store_key_image_to_rec(Field *field, uchar *ptr, uint len);

int ha_cassandra::index_read_map(uchar *buf, const uchar *key,
                               key_part_map keypart_map,
                               enum ha_rkey_function find_flag)
{
  int rc;
  DBUG_ENTER("ha_cassandra::index_read_map");
  
  if (find_flag != HA_READ_KEY_EXACT)
    DBUG_RETURN(HA_ERR_WRONG_COMMAND);

  // todo: decode the search key.
  uint key_len= calculate_key_len(table, active_index, key, keypart_map);
  store_key_image_to_rec(table->field[0], (uchar*)key, key_len);

  char buff[256]; 
  String tmp(buff,sizeof(buff), &my_charset_bin);
  tmp.length(0);
  String *str;
  str= table->field[0]->val_str(&tmp);
  
  bool found;
  if (se->get_slice((char*)str->ptr(), str->length(), get_names_and_vals(), &found))
    rc= HA_ERR_INTERNAL_ERROR;
  else
  {
    if (found)
    {
      //NameAndValue *nv= get_names_and_vals();
      // TODO: walk through the (name, value) pairs and return values.
    }
    else
      rc= HA_ERR_KEY_NOT_FOUND;
  }
#ifdef NEW_CODE
  
  se->get_slice();

  for each column
  {
    find column;
  }
#endif

  DBUG_RETURN(rc);
}


int ha_cassandra::write_row(uchar *buf)
{
  my_bitmap_map *old_map;
  char buff[512]; 
  NameAndValue *tuple;
  NameAndValue *nv;
  DBUG_ENTER("ha_cassandra::write_row");
  
  /* Temporary malloc-happy code just to get INSERTs to work */
  nv= tuple= get_names_and_vals();
  old_map= dbug_tmp_use_all_columns(table, table->read_set);

  for (Field **field= table->field; *field; field++, nv++)
  {
    String tmp(buff,sizeof(buff), &my_charset_bin);
    tmp.length(0);
    String *str;
    str= (*field)->val_str(&tmp);
    nv->name= (char*)(*field)->field_name;
    nv->value_len= str->length();
    nv->value= (char*)my_malloc(nv->value_len, MYF(0));
    memcpy(nv->value, str->ptr(), nv->value_len);
  }
  nv->name= NULL;
  dbug_tmp_restore_column_map(table->read_set, old_map);
  
  //invoke!
  bool res= se->insert(tuple);

  for (nv= tuple; nv->name; nv++)
  {
    my_free(nv->value);
  }

  DBUG_RETURN(res? HA_ERR_INTERNAL_ERROR: 0);
}


NameAndValue *ha_cassandra::get_names_and_vals()
{
  if (names_and_vals)
    return names_and_vals;
  else
  {
    size_t size= sizeof(NameAndValue) * (table->s->fields + 1);
    names_and_vals= (NameAndValue*)my_malloc(size ,0);
    memset(names_and_vals, 0, size);
    return names_and_vals;
  }
}


/////////////////////////////////////////////////////////////////////////////
// Dummy implementations start
/////////////////////////////////////////////////////////////////////////////


int ha_cassandra::index_next(uchar *buf)
{
  int rc;
  DBUG_ENTER("ha_cassandra::index_next");
  rc= HA_ERR_WRONG_COMMAND;
  DBUG_RETURN(rc);
}


int ha_cassandra::index_prev(uchar *buf)
{
  int rc;
  DBUG_ENTER("ha_cassandra::index_prev");
  rc= HA_ERR_WRONG_COMMAND;
  DBUG_RETURN(rc);
}


int ha_cassandra::index_first(uchar *buf)
{
  int rc;
  DBUG_ENTER("ha_cassandra::index_first");
  rc= HA_ERR_WRONG_COMMAND;
  DBUG_RETURN(rc);
}

int ha_cassandra::index_last(uchar *buf)
{
  int rc;
  DBUG_ENTER("ha_cassandra::index_last");
  rc= HA_ERR_WRONG_COMMAND;
  DBUG_RETURN(rc);
}

int ha_cassandra::rnd_init(bool scan)
{
  DBUG_ENTER("ha_cassandra::rnd_init");
  DBUG_RETURN(0);
}

int ha_cassandra::rnd_end()
{
  DBUG_ENTER("ha_cassandra::rnd_end");
  DBUG_RETURN(0);
}


int ha_cassandra::rnd_next(uchar *buf)
{
  int rc;
  DBUG_ENTER("ha_cassandra::rnd_next");
  rc= HA_ERR_END_OF_FILE;
  DBUG_RETURN(rc);
}

void ha_cassandra::position(const uchar *record)
{
  DBUG_ENTER("ha_cassandra::position");
  DBUG_VOID_RETURN;
}

int ha_cassandra::rnd_pos(uchar *buf, uchar *pos)
{
  int rc;
  DBUG_ENTER("ha_cassandra::rnd_pos");
  rc= HA_ERR_WRONG_COMMAND;
  DBUG_RETURN(rc);
}

ha_rows ha_cassandra::records_in_range(uint inx, key_range *min_key,
                                     key_range *max_key)
{
  DBUG_ENTER("ha_cassandra::records_in_range");
  DBUG_RETURN(10);                         // low number to force index usage
}


int ha_cassandra::update_row(const uchar *old_data, uchar *new_data)
{

  DBUG_ENTER("ha_cassandra::update_row");
  DBUG_RETURN(HA_ERR_WRONG_COMMAND);
}


int ha_cassandra::info(uint flag)
{
  DBUG_ENTER("ha_cassandra::info");
  DBUG_RETURN(0);
}


int ha_cassandra::extra(enum ha_extra_function operation)
{
  DBUG_ENTER("ha_cassandra::extra");
  DBUG_RETURN(0);
}


THR_LOCK_DATA **ha_cassandra::store_lock(THD *thd,
                                       THR_LOCK_DATA **to,
                                       enum thr_lock_type lock_type)
{
  if (lock_type != TL_IGNORE && lock.type == TL_UNLOCK)
    lock.type=lock_type;
  *to++= &lock;
  return to;
}


int ha_cassandra::external_lock(THD *thd, int lock_type)
{
  DBUG_ENTER("ha_cassandra::external_lock");
  DBUG_RETURN(0);
}

int ha_cassandra::delete_table(const char *name)
{
  DBUG_ENTER("ha_cassandra::delete_table");
  /* This is not implemented but we want someone to be able that it works. */
  DBUG_RETURN(0);
}


int ha_cassandra::delete_row(const uchar *buf)
{
  DBUG_ENTER("ha_cassandra::delete_row");
  DBUG_RETURN(HA_ERR_WRONG_COMMAND);
}


int ha_cassandra::delete_all_rows()
{
  DBUG_ENTER("ha_cassandra::delete_all_rows");
  DBUG_RETURN(HA_ERR_WRONG_COMMAND);
}


/**
  check_if_incompatible_data() called if ALTER TABLE can't detect otherwise
  if new and old definition are compatible

  @details If there are no other explicit signs like changed number of
  fields this function will be called by compare_tables()
  (sql/sql_tables.cc) to decide should we rewrite whole table or only .frm
  file.

*/

bool ha_cassandra::check_if_incompatible_data(HA_CREATE_INFO *info,
                                            uint table_changes)
{
  //ha_table_option_struct *param_old, *param_new;
  DBUG_ENTER("ha_cassandra::check_if_incompatible_data");

  DBUG_RETURN(COMPATIBLE_DATA_YES);
}


/////////////////////////////////////////////////////////////////////////////
// Dummy implementations end
/////////////////////////////////////////////////////////////////////////////


static struct st_mysql_sys_var* cassandra_system_variables[]= {
//  MYSQL_SYSVAR(enum_var),
//  MYSQL_SYSVAR(ulong_var),
  NULL
};


struct st_mysql_storage_engine cassandra_storage_engine=
{ MYSQL_HANDLERTON_INTERFACE_VERSION };

static struct st_mysql_show_var func_status[]=
{
//  {"example_func_example",  (char *)show_func_example, SHOW_FUNC},
  {0,0,SHOW_UNDEF}
};

maria_declare_plugin(cassandra)
{
  MYSQL_STORAGE_ENGINE_PLUGIN,
  &cassandra_storage_engine,
  "CASSANDRA",
  "Monty Program Ab",
  "Cassandra storage engine",
  PLUGIN_LICENSE_GPL,
  cassandra_init_func,                            /* Plugin Init */
  cassandra_done_func,                            /* Plugin Deinit */
  0x0001,                                       /* version number (0.1) */
  func_status,                                  /* status variables */
  cassandra_system_variables,                     /* system variables */
  "0.1",                                        /* string version */
  MariaDB_PLUGIN_MATURITY_EXPERIMENTAL          /* maturity */
}
maria_declare_plugin_end;
