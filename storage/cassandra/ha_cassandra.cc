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
   se(NULL), field_converters(NULL), rowkey_converter(NULL),
   rnd_batch_size(10*1000)
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

  if (setup_field_converters(table->field, table->s->fields))
  {
    DBUG_RETURN(HA_ERR_NO_CONNECTION);
  }

  info(HA_STATUS_NO_LOCK | HA_STATUS_VARIABLE | HA_STATUS_CONST);

  DBUG_RETURN(0);
}


int ha_cassandra::close(void)
{
  DBUG_ENTER("ha_cassandra::close");
  delete se;
  se= NULL;
  free_field_converters();
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

  if (!((*pfield)->flags & NOT_NULL_FLAG))
  {
    my_error(ER_WRONG_COLUMN_NAME, MYF(0), "First column must be NOT NULL");
    DBUG_RETURN(HA_WRONG_CREATE_OPTION);
  }

  if (table_arg->s->keys != 1 || table_arg->s->primary_key !=0 ||
      table_arg->key_info[0].key_parts != 1 ||
      table_arg->key_info[0].key_part[0].fieldnr != 1)
  {
    my_error(ER_WRONG_COLUMN_NAME, MYF(0), "Table must have one PRIMARY KEY(rowkey)");
    DBUG_RETURN(HA_WRONG_CREATE_OPTION);
  }


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
  if (!options->host  || !options->keyspace || !options->column_family)
  {
    my_error(ER_CONNECT_TO_FOREIGN_DATA_SOURCE, MYF(0), 
             "thrift_host, keyspace, and column_family table options must be specified");
    DBUG_RETURN(HA_WRONG_CREATE_OPTION);
  }
  se= get_cassandra_se();
  se->set_column_family(options->column_family);
  if (se->connect(options->host, options->keyspace))
  {
    my_error(ER_CONNECT_TO_FOREIGN_DATA_SOURCE, MYF(0), se->error_str());
    DBUG_RETURN(HA_ERR_NO_CONNECTION);
  }
  
  if (setup_field_converters(table_arg->s->field, table_arg->s->fields))
  {
    my_error(ER_CONNECT_TO_FOREIGN_DATA_SOURCE, MYF(0), "setup_field_converters");
    DBUG_RETURN(HA_ERR_NO_CONNECTION);
  }
  DBUG_RETURN(0);
}

/*
  Mapping needs to
  - copy value from MySQL record to Thrift buffer
  - copy value from Thrift bufer to MySQL record..

*/

/* Converter base */
class ColumnDataConverter
{
public:
  Field *field;

  /* This will save Cassandra's data in the Field */
  virtual void cassandra_to_mariadb(const char *cass_data, 
                                    int cass_data_len)=0;

  /*
    This will get data from the Field pointer, store Cassandra's form
    in internal buffer, and return pointer/size.
  */
  virtual void mariadb_to_cassandra(char **cass_data, int *cass_data_len)=0;
  virtual ~ColumnDataConverter() {};
};


class DoubleDataConverter : public ColumnDataConverter
{
  double buf;
public:
  void cassandra_to_mariadb(const char *cass_data, int cass_data_len)
  {
    DBUG_ASSERT(cass_data_len == sizeof(double));
    double *pdata= (double*) cass_data;
    field->store(*pdata);
  }
  
  void mariadb_to_cassandra(char **cass_data, int *cass_data_len)
  {
    buf= field->val_real();
    *cass_data= (char*)&buf;
    *cass_data_len=sizeof(double);
  }
  ~DoubleDataConverter(){}
};


class FloatDataConverter : public ColumnDataConverter
{
  float buf;
public:
  void cassandra_to_mariadb(const char *cass_data, int cass_data_len)
  {
    DBUG_ASSERT(cass_data_len == sizeof(float));
    float *pdata= (float*) cass_data;
    field->store(*pdata);
  }
  
  void mariadb_to_cassandra(char **cass_data, int *cass_data_len)
  {
    buf= field->val_real();
    *cass_data= (char*)&buf;
    *cass_data_len=sizeof(float);
  }
  ~FloatDataConverter(){}
};


class BigintDataConverter : public ColumnDataConverter
{
  longlong buf;
public:
  void flip(const char *from, char* to)
  {
    to[0]= from[7];
    to[1]= from[6];
    to[2]= from[5];
    to[3]= from[4];
    to[4]= from[3];
    to[5]= from[2];
    to[6]= from[1];
    to[7]= from[0];
  }
  void cassandra_to_mariadb(const char *cass_data, int cass_data_len)
  {
    longlong tmp;
    DBUG_ASSERT(cass_data_len == sizeof(longlong));
    flip(cass_data, (char*)&tmp);
    field->store(tmp);
  }
  
  void mariadb_to_cassandra(char **cass_data, int *cass_data_len)
  {
    longlong tmp= field->val_int();
    flip((const char*)&tmp, (char*)&buf);
    *cass_data= (char*)&buf;
    *cass_data_len=sizeof(longlong);
  }
  ~BigintDataConverter(){}
};


class StringCopyConverter : public ColumnDataConverter
{
  String buf;
public:
  void cassandra_to_mariadb(const char *cass_data, int cass_data_len)
  {
    field->store(cass_data, cass_data_len,field->charset());
  }
  
  void mariadb_to_cassandra(char **cass_data, int *cass_data_len)
  {
    String *pstr= field->val_str(&buf);
    *cass_data= (char*)pstr->c_ptr();
    *cass_data_len= pstr->length();
  }
  ~StringCopyConverter(){}
};


const char * const validator_bigint=  "org.apache.cassandra.db.marshal.LongType";
const char * const validator_int=     "org.apache.cassandra.db.marshal.Int32Type";
const char * const validator_counter= "org.apache.cassandra.db.marshal.CounterColumnType";

const char * const validator_float=   "org.apache.cassandra.db.marshal.FloatType";
const char * const validator_double=  "org.apache.cassandra.db.marshal.DoubleType";

const char * const validator_blob=    "org.apache.cassandra.db.marshal.BytesType";
const char * const validator_ascii=   "org.apache.cassandra.db.marshal.AsciiType";
const char * const validator_text=    "org.apache.cassandra.db.marshal.UTF8Type";


ColumnDataConverter *map_field_to_validator(Field *field, const char *validator_name)
{
  ColumnDataConverter *res= NULL;

  switch(field->type()) {
    case MYSQL_TYPE_TINY:
    case MYSQL_TYPE_SHORT:
    case MYSQL_TYPE_LONG:
    case MYSQL_TYPE_LONGLONG:
      if (!strcmp(validator_name, validator_bigint))
        res= new BigintDataConverter;
      break;
    case MYSQL_TYPE_FLOAT:
      if (!strcmp(validator_name, validator_float))
        res= new FloatDataConverter;
      break;
    case MYSQL_TYPE_DOUBLE:
      if (!strcmp(validator_name, validator_double))
        res= new DoubleDataConverter;
      break;

    case MYSQL_TYPE_VAR_STRING:
    case MYSQL_TYPE_VARCHAR:
    //case MYSQL_TYPE_STRING:  <-- todo: should we allow end-padded 'CHAR(N)'?
      if (!strcmp(validator_name, validator_blob) ||
          !strcmp(validator_name, validator_ascii) ||
          !strcmp(validator_name, validator_text))
      {
        res= new StringCopyConverter;
      }
      break;
    default:;
  }
  return res;
}


bool ha_cassandra::setup_field_converters(Field **field_arg, uint n_fields)
{
  char *col_name;
  int  col_name_len;
  char *col_type;
  int col_type_len;

  DBUG_ASSERT(!field_converters);
  size_t memsize= sizeof(ColumnDataConverter*) * n_fields;
  if (!(field_converters= (ColumnDataConverter**)my_malloc(memsize, MYF(0))))
    return true;
  bzero(field_converters, memsize);
  n_field_converters= n_fields;

  /* 
    TODO: what about mapping the primary key? It has a 'type', too...
    see CfDef::key_validation_class ? see also CfDef::key_alias?
  */

  se->first_ddl_column();
  uint n_mapped= 0;
  while (!se->next_ddl_column(&col_name, &col_name_len, &col_type,
                              &col_type_len))
  {
    /* Mapping for the 1st field is already known */
    for (Field **field= field_arg + 1; *field; field++)
    {
      if (!strcmp((*field)->field_name, col_name))
      {
        n_mapped++;
        ColumnDataConverter **conv= field_converters + (*field)->field_index;
        if (!(*conv= map_field_to_validator(*field, col_type)))
        {
          se->print_error("Failed to map column %s to datatype %s", 
                          (*field)->field_name, col_type);
          my_error(ER_INTERNAL_ERROR, MYF(0), se->error_str());
          return true;
        }
        (*conv)->field= *field;
      }
    }
  }

  if (n_mapped != n_fields - 1)
    return true;
  
  /* 
    Setup type conversion for row_key. It may also have a name, but we ignore
    it currently
  */
  se->get_rowkey_type(&col_name, &col_type);
  if (col_type != NULL)
  {
    if (!(rowkey_converter= map_field_to_validator(*field_arg, col_type)))
    {
      se->print_error("Failed to map PRIMARY KEY to datatype %s", col_type);
      my_error(ER_INTERNAL_ERROR, MYF(0), se->error_str());
      return true;
    }
    rowkey_converter->field= *field_arg;
  }
  else
  {
    se->print_error("Cassandra's rowkey has no defined datatype (todo: support this)");
    my_error(ER_INTERNAL_ERROR, MYF(0), se->error_str());
    return true;
  }

  return false;
}


void ha_cassandra::free_field_converters()
{
  delete rowkey_converter;
  rowkey_converter= NULL;

  if (field_converters)
  {
    for (uint i=0; i < n_field_converters; i++)
      delete field_converters[i];
    my_free(field_converters);
    field_converters= NULL;
  }
}


void store_key_image_to_rec(Field *field, uchar *ptr, uint len);

int ha_cassandra::index_read_map(uchar *buf, const uchar *key,
                                 key_part_map keypart_map,
                                 enum ha_rkey_function find_flag)
{
  int rc= 0;
  DBUG_ENTER("ha_cassandra::index_read_map");
  
  if (find_flag != HA_READ_KEY_EXACT)
    DBUG_RETURN(HA_ERR_WRONG_COMMAND);

  uint key_len= calculate_key_len(table, active_index, key, keypart_map);
  store_key_image_to_rec(table->field[0], (uchar*)key, key_len);

  char *cass_key;
  int cass_key_len;
  my_bitmap_map *old_map;

  old_map= dbug_tmp_use_all_columns(table, table->read_set);

  rowkey_converter->mariadb_to_cassandra(&cass_key, &cass_key_len);

  dbug_tmp_restore_column_map(table->read_set, old_map);

  bool found;
  if (se->get_slice(cass_key, cass_key_len, &found))
  {
    my_error(ER_INTERNAL_ERROR, MYF(0), se->error_str());
    rc= HA_ERR_INTERNAL_ERROR;
  }
  
  /* TODO: what if we're not reading all columns?? */
  if (!found)
  {
    rc= HA_ERR_KEY_NOT_FOUND;
  }
  else
  {
    read_cassandra_columns(false);
  }

  DBUG_RETURN(rc);
}


void ha_cassandra::read_cassandra_columns(bool unpack_pk)
{
  char *cass_name;
  char *cass_value;
  int cass_value_len;
  Field **field;
  
  /* 
    cassandra_to_mariadb() calls will use field->store(...) methods, which
    require that the column is in the table->write_set
  */
  my_bitmap_map *old_map;
  old_map= dbug_tmp_use_all_columns(table, table->write_set);

  /* Start with all fields being NULL */
  for (field= table->field + 1; *field; field++)
    (*field)->set_null();

  while (!se->get_next_read_column(&cass_name, &cass_value, &cass_value_len))
  {
    // map to our column. todo: use hash or something..
    int idx=1;
    for (field= table->field + 1; *field; field++)
    {
      idx++;
      if (!strcmp((*field)->field_name, cass_name))
      {
        int fieldnr= (*field)->field_index;
        (*field)->set_notnull();
        field_converters[fieldnr]->cassandra_to_mariadb(cass_value, cass_value_len);
        break;
      }
    }
  }
  
  if (unpack_pk)
  {
    /* Unpack rowkey to primary key */
    field= table->field;
    (*field)->set_notnull();
    se->get_read_rowkey(&cass_value, &cass_value_len);
    rowkey_converter->cassandra_to_mariadb(cass_value, cass_value_len);
  }

  dbug_tmp_restore_column_map(table->write_set, old_map);
}


int ha_cassandra::write_row(uchar *buf)
{
  my_bitmap_map *old_map;
  DBUG_ENTER("ha_cassandra::write_row");
  
  old_map= dbug_tmp_use_all_columns(table, table->read_set);
  
  /* Convert the key */
  char *cass_key;
  int cass_key_len;
  rowkey_converter->mariadb_to_cassandra(&cass_key, &cass_key_len);
  se->start_prepare_insert(cass_key, cass_key_len);

  /* Convert other fields */
  for (uint i= 1; i < table->s->fields; i++)
  {
    char *cass_data;
    int cass_data_len;
    field_converters[i]->mariadb_to_cassandra(&cass_data, &cass_data_len);
    se->add_insert_column(field_converters[i]->field->field_name, 
                          cass_data, cass_data_len);
  }

  dbug_tmp_restore_column_map(table->read_set, old_map);
  
  bool res= se->do_insert();

  if (res)
    my_error(ER_INTERNAL_ERROR, MYF(0), se->error_str());

  DBUG_RETURN(res? HA_ERR_INTERNAL_ERROR: 0);
}


int ha_cassandra::rnd_init(bool scan)
{
  bool bres;
  DBUG_ENTER("ha_cassandra::rnd_init");
  if (!scan)
  {
    /* Prepare for rnd_pos() calls. We don't need to anything. */
    DBUG_RETURN(0);
  }

  se->clear_read_columns();
  for (uint i= 1; i < table->s->fields; i++)
    se->add_read_column(table->field[i]->field_name);

  se->read_batch_size= rnd_batch_size;
  bres= se->get_range_slices(false);
  if (bres)
    my_error(ER_INTERNAL_ERROR, MYF(0), se->error_str());

  DBUG_RETURN(bres? HA_ERR_INTERNAL_ERROR: 0);
}


int ha_cassandra::rnd_end()
{
  DBUG_ENTER("ha_cassandra::rnd_end");

  se->finish_reading_range_slices();
  DBUG_RETURN(0);
}


int ha_cassandra::rnd_next(uchar *buf)
{
  int rc;
  bool reached_eof;
  DBUG_ENTER("ha_cassandra::rnd_next");

  // Unpack and return the next record.
  if (se->get_next_range_slice_row(&reached_eof))
  {
    rc= HA_ERR_INTERNAL_ERROR;
  }
  else
  {
    if (reached_eof)
      rc= HA_ERR_END_OF_FILE;
    else
    {
      read_cassandra_columns(true);
      rc= 0;
    }
  }

  DBUG_RETURN(rc);
}


int ha_cassandra::delete_all_rows()
{
  bool bres;
  DBUG_ENTER("ha_cassandra::delete_all_rows");

  bres= se->truncate();
  
  if (bres)
    my_error(ER_INTERNAL_ERROR, MYF(0), se->error_str());

  DBUG_RETURN(bres? HA_ERR_INTERNAL_ERROR: 0);
}


int ha_cassandra::delete_row(const uchar *buf)
{
  bool bres;
  DBUG_ENTER("ha_cassandra::delete_row");
  
  bres= se->remove_row();
  
  if (bres)
    my_error(ER_INTERNAL_ERROR, MYF(0), se->error_str());
  
  DBUG_RETURN(bres? HA_ERR_INTERNAL_ERROR: 0);
}


int ha_cassandra::info(uint flag)
{
  DBUG_ENTER("ha_cassandra::info");
  
  if (!table)
    return 1;

  if (flag & HA_STATUS_VARIABLE)
  {
    stats.records= 1000;
    //TODO: any other stats?
  }
  if (flag & HA_STATUS_CONST)
  {
    ref_length= table->field[0]->key_length();
  }

  DBUG_RETURN(0);
}


void key_copy(uchar *to_key, uchar *from_record, KEY *key_info,
              uint key_length, bool with_zerofill);


void ha_cassandra::position(const uchar *record)
{
  DBUG_ENTER("ha_cassandra::position");
  
  /* Copy the primary key to rowid */
  key_copy(ref, (uchar*)record, &table->key_info[0],
           table->field[0]->key_length(), true);

  DBUG_VOID_RETURN;
}


int ha_cassandra::rnd_pos(uchar *buf, uchar *pos)
{
  int rc;
  DBUG_ENTER("ha_cassandra::rnd_pos");
  
  int save_active_index= active_index;
  active_index= 0; /* The primary key */
  rc= index_read_map(buf, pos, key_part_map(1), HA_READ_KEY_EXACT);

  active_index= save_active_index;

  DBUG_RETURN(rc);
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


ha_rows ha_cassandra::records_in_range(uint inx, key_range *min_key,
                                     key_range *max_key)
{
  DBUG_ENTER("ha_cassandra::records_in_range");
  //DBUG_RETURN(10);                         // low number to force index usage
  DBUG_RETURN(HA_POS_ERROR);
}


int ha_cassandra::update_row(const uchar *old_data, uchar *new_data)
{

  DBUG_ENTER("ha_cassandra::update_row");
  DBUG_RETURN(HA_ERR_WRONG_COMMAND);
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
  /* 
    Cassandra table is just a view. Dropping it doesn't affect the underlying
    column family.
  */
  DBUG_RETURN(0);
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
