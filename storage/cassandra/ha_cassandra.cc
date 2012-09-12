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


static MYSQL_THDVAR_ULONG(insert_batch_size, PLUGIN_VAR_RQCMDARG,
  "Number of rows in an INSERT batch",
  NULL, NULL, /*default*/ 100, /*min*/ 1, /*max*/ 1024*1024*1024, 0);

static MYSQL_THDVAR_ULONG(multiget_batch_size, PLUGIN_VAR_RQCMDARG,
  "Number of rows in a multiget(MRR) batch",
  NULL, NULL, /*default*/ 100, /*min*/ 1, /*max*/ 1024*1024*1024, 0);

static MYSQL_THDVAR_ULONG(rnd_batch_size, PLUGIN_VAR_RQCMDARG,
  "Number of rows in an rnd_read (full scan) batch",
  NULL, NULL, /*default*/ 10*1000, /*min*/ 1, /*max*/ 1024*1024*1024, 0);

static struct st_mysql_sys_var* cassandra_system_variables[]= {
  MYSQL_SYSVAR(insert_batch_size),
  MYSQL_SYSVAR(multiget_batch_size),
  MYSQL_SYSVAR(rnd_batch_size),
//  MYSQL_SYSVAR(enum_var),
//  MYSQL_SYSVAR(ulong_var),
  NULL
};


static SHOW_VAR cassandra_status_variables[]= {
  {"row_inserts",
    (char*) &cassandra_counters.row_inserts,         SHOW_LONG},
  {"row_insert_batches",
    (char*) &cassandra_counters.row_insert_batches,  SHOW_LONG},

  {"multiget_reads",
    (char*) &cassandra_counters.multiget_reads,      SHOW_LONG},
  {"multiget_keys_scanned",
    (char*) &cassandra_counters.multiget_keys_scanned, SHOW_LONG},
  {"multiget_rows_read",
    (char*) &cassandra_counters.multiget_rows_read,  SHOW_LONG},
  {NullS, NullS, SHOW_LONG}
};


Cassandra_status_vars cassandra_counters;
Cassandra_status_vars cassandra_counters_copy;


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
  /* 
    Don't specify HTON_CAN_RECREATE in flags. re-create is used by TRUNCATE
    TABLE to create an *empty* table from scratch. Cassandra table won't be
    emptied if re-created.
  */
  cassandra_hton->flags=   0; 
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
   se(NULL), field_converters(NULL), rowkey_converter(NULL)
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
  insert_lineno= 0;

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
  

  Field **pfield= table_arg->s->field;
/*  
  if (strcmp((*pfield)->field_name, "rowkey"))
  {
    my_error(ER_WRONG_COLUMN_NAME, MYF(0), "First column must be named 'rowkey'");
    DBUG_RETURN(HA_WRONG_CREATE_OPTION);
  }
*/
  if (!((*pfield)->flags & NOT_NULL_FLAG))
  {
    my_error(ER_WRONG_COLUMN_NAME, MYF(0), "First column must be NOT NULL");
    DBUG_RETURN(HA_WRONG_CREATE_OPTION);
  }

  if (table_arg->s->keys != 1 || table_arg->s->primary_key !=0 ||
      table_arg->key_info[0].key_parts != 1 ||
      table_arg->key_info[0].key_part[0].fieldnr != 1)
  {
    my_error(ER_WRONG_COLUMN_NAME, MYF(0), 
             "Table must have PRIMARY KEY defined over the first column");
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
  insert_lineno= 0;
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

    @return
      false - OK
      true  - Failed to convert value (completely, there is no value to insert
              at all).
  */
  virtual bool mariadb_to_cassandra(char **cass_data, int *cass_data_len)=0;
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
  
  bool mariadb_to_cassandra(char **cass_data, int *cass_data_len)
  {
    buf= field->val_real();
    *cass_data= (char*)&buf;
    *cass_data_len=sizeof(double);
    return false;
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
  
  bool mariadb_to_cassandra(char **cass_data, int *cass_data_len)
  {
    buf= field->val_real();
    *cass_data= (char*)&buf;
    *cass_data_len=sizeof(float);
    return false;
  }
  ~FloatDataConverter(){}
};

static void flip64(const char *from, char* to)
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

class BigintDataConverter : public ColumnDataConverter
{
  longlong buf;
  bool flip; /* is false when reading counter columns */
public:
  void cassandra_to_mariadb(const char *cass_data, int cass_data_len)
  {
    longlong tmp;
    DBUG_ASSERT(cass_data_len == sizeof(longlong));
    if (flip)
      flip64(cass_data, (char*)&tmp);
    else
      memcpy(&tmp, cass_data, sizeof(longlong));
    field->store(tmp);
  }
  
  bool mariadb_to_cassandra(char **cass_data, int *cass_data_len)
  {
    longlong tmp= field->val_int();
    if (flip)
      flip64((const char*)&tmp, (char*)&buf);
    else
      memcpy(&buf, &tmp, sizeof(longlong));
    *cass_data= (char*)&buf;
    *cass_data_len=sizeof(longlong);
    return false;
  }
  BigintDataConverter(bool flip_arg) : flip(flip_arg) {}
  ~BigintDataConverter(){}
};

static void flip32(const char *from, char* to)
{
  to[0]= from[3];
  to[1]= from[2];
  to[2]= from[1];
  to[3]= from[0];
}


class TinyintDataConverter : public ColumnDataConverter
{
  char buf;
public:
  void cassandra_to_mariadb(const char *cass_data, int cass_data_len)
  {
    DBUG_ASSERT(cass_data_len == 1);
    field->store(cass_data[0]);
  }

  bool mariadb_to_cassandra(char **cass_data, int *cass_data_len)
  {
    buf= field->val_int()? 1 : 0; /* TODO: error handling? */
    *cass_data= (char*)&buf;
    *cass_data_len= 1;
    return false;
  }
  ~TinyintDataConverter(){}
};


class Int32DataConverter : public ColumnDataConverter
{
  int32_t buf;
public:
  void cassandra_to_mariadb(const char *cass_data, int cass_data_len)
  {
    int32_t tmp;
    DBUG_ASSERT(cass_data_len == sizeof(int32_t));
    flip32(cass_data, (char*)&tmp);
    field->store(tmp);
  }
  
  bool mariadb_to_cassandra(char **cass_data, int *cass_data_len)
  {
    int32_t tmp= field->val_int();
    flip32((const char*)&tmp, (char*)&buf);
    *cass_data= (char*)&buf;
    *cass_data_len=sizeof(int32_t);
    return false;
  }
  ~Int32DataConverter(){}
};


class StringCopyConverter : public ColumnDataConverter
{
  String buf;
public:
  void cassandra_to_mariadb(const char *cass_data, int cass_data_len)
  {
    field->store(cass_data, cass_data_len,field->charset());
  }
  
  bool mariadb_to_cassandra(char **cass_data, int *cass_data_len)
  {
    String *pstr= field->val_str(&buf);
    *cass_data= (char*)pstr->ptr();
    *cass_data_len= pstr->length();
    return false;
  }
  ~StringCopyConverter(){}
};


class TimestampDataConverter : public ColumnDataConverter
{
  int64_t buf;
public:
  void cassandra_to_mariadb(const char *cass_data, int cass_data_len)
  {
    /* Cassandra data is milliseconds-since-epoch in network byte order */
    int64_t tmp;
    DBUG_ASSERT(cass_data_len==8);
    flip64(cass_data, (char*)&tmp);
    /*
      store_TIME's arguments: 
      - seconds since epoch
      - microsecond fraction of a second.
    */
    ((Field_timestamp*)field)->store_TIME(tmp / 1000, (tmp % 1000)*1000);
  }

  bool mariadb_to_cassandra(char **cass_data, int *cass_data_len)
  {
    my_time_t ts_time;
    ulong ts_microsec;
    int64_t tmp;
    ts_time= ((Field_timestamp*)field)->get_timestamp(&ts_microsec);
    
    /* Cassandra needs milliseconds-since-epoch */
    tmp= ((int64_t)ts_time) * 1000 + ts_microsec/1000;
    flip64((const char*)&tmp, (char*)&buf);

    *cass_data= (char*)&buf;
    *cass_data_len= 8;
    return false;
  }
  ~TimestampDataConverter(){}
};



static int convert_hex_digit(const char c)
{
  int num;
  if (c >= '0' && c <= '9')
    num= c - '0';
  else if (c >= 'A' && c <= 'F')
    num= c - 'A' + 10;
  else if (c >= 'a' && c <= 'f')
    num= c - 'a' + 10;
  else
    return -1; /* Couldn't convert */
  return num;
}


const char map2number[]="0123456789abcdef";

class UuidDataConverter : public ColumnDataConverter
{
  char buf[16]; /* Binary UUID representation */
  String str_buf;
public:
  void cassandra_to_mariadb(const char *cass_data, int cass_data_len)
  {
    DBUG_ASSERT(cass_data_len==16);
    char str[37];
    char *ptr= str;
    /* UUID arrives as 16-byte number in network byte order */
    for (uint i=0; i < 16; i++)
    {
      *(ptr++)= map2number[(cass_data[i] >> 4) & 0xF];
      *(ptr++)= map2number[cass_data[i] & 0xF];
      if (i == 3 || i == 5 || i == 7 || i == 9)
        *(ptr++)= '-';
    }
    *ptr= 0;
    field->store(str, 36,field->charset());
  }

  bool mariadb_to_cassandra(char **cass_data, int *cass_data_len)
  {
    String *uuid_str= field->val_str(&str_buf);
    char *pstr= (char*)uuid_str->c_ptr();

    if (uuid_str->length() != 36) 
      return true;
    
    int lower, upper;
    for (uint i=0; i < 16; i++)
    {
      if ((upper= convert_hex_digit(pstr[0])) == -1 ||
          (lower= convert_hex_digit(pstr[1])) == -1)
      {
        return true;
      }
      buf[i]= lower | (upper << 4);
      pstr += 2;
      if (i == 3 || i == 5 || i == 7 || i == 9)
      {
        if (pstr[0] != '-')
          return true;
        pstr++;
      }
    }
     
    *cass_data= buf;
    *cass_data_len= 16;
    return false;
  }
  ~UuidDataConverter(){}
};


const char * const validator_bigint=  "org.apache.cassandra.db.marshal.LongType";
const char * const validator_int=     "org.apache.cassandra.db.marshal.Int32Type";
const char * const validator_counter= "org.apache.cassandra.db.marshal.CounterColumnType";

const char * const validator_float=   "org.apache.cassandra.db.marshal.FloatType";
const char * const validator_double=  "org.apache.cassandra.db.marshal.DoubleType";

const char * const validator_blob=    "org.apache.cassandra.db.marshal.BytesType";
const char * const validator_ascii=   "org.apache.cassandra.db.marshal.AsciiType";
const char * const validator_text=    "org.apache.cassandra.db.marshal.UTF8Type";

const char * const validator_timestamp="org.apache.cassandra.db.marshal.DateType";

const char * const validator_uuid= "org.apache.cassandra.db.marshal.UUIDType";

const char * const validator_boolean= "org.apache.cassandra.db.marshal.BooleanType";


ColumnDataConverter *map_field_to_validator(Field *field, const char *validator_name)
{
  ColumnDataConverter *res= NULL;

  switch(field->type()) {
    case MYSQL_TYPE_TINY:
      if (!strcmp(validator_name, validator_boolean))
      {
        res= new TinyintDataConverter;
        break;
      }
      /* fall through: */
    case MYSQL_TYPE_SHORT:
    case MYSQL_TYPE_LONGLONG:
    {
      bool is_counter= false;
      if (!strcmp(validator_name, validator_bigint) ||
          (is_counter= !strcmp(validator_name, validator_counter)))
        res= new BigintDataConverter(!is_counter);
      break;
    }
    case MYSQL_TYPE_FLOAT:
      if (!strcmp(validator_name, validator_float))
        res= new FloatDataConverter;
      break;

    case MYSQL_TYPE_DOUBLE:
      if (!strcmp(validator_name, validator_double))
        res= new DoubleDataConverter;
      break;
    
    case MYSQL_TYPE_TIMESTAMP:
      if (!strcmp(validator_name, validator_timestamp))
        res= new TimestampDataConverter;
      break;

    case MYSQL_TYPE_STRING: // these are space padded CHAR(n) strings.
      if (!strcmp(validator_name, validator_uuid) && 
          field->real_type() == MYSQL_TYPE_STRING &&
          field->field_length == 36) 
      {
        // UUID maps to CHAR(36), its text representation
        res= new UuidDataConverter;
        break;
      }
      /* fall through: */
    case MYSQL_TYPE_VARCHAR:
    case MYSQL_TYPE_VAR_STRING:
      if (!strcmp(validator_name, validator_blob) ||
          !strcmp(validator_name, validator_ascii) ||
          !strcmp(validator_name, validator_text))
      {
        res= new StringCopyConverter;
      }
      break;

    case MYSQL_TYPE_LONG:
      if (!strcmp(validator_name, validator_int))
        res= new Int32DataConverter;
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
  {
    se->print_error("Some of SQL fields were not mapped to Cassandra's fields"); 
    my_error(ER_INTERNAL_ERROR, MYF(0), se->error_str());
    return true;
  }
  
  /* 
    Setup type conversion for row_key.
  */
  se->get_rowkey_type(&col_name, &col_type);
  if (col_name && strcmp(col_name, (*field_arg)->field_name))
  {
    se->print_error("PRIMARY KEY column must match Cassandra's name '%s'", col_name);
    my_error(ER_INTERNAL_ERROR, MYF(0), se->error_str());
    return true;
  }
  if (!col_name && strcmp("rowkey", (*field_arg)->field_name))
  {
    se->print_error("target column family has no key_alias defined, "
                    "PRIMARY KEY column must be named 'rowkey'");
    my_error(ER_INTERNAL_ERROR, MYF(0), se->error_str());
    return true;
  }

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

  if (rowkey_converter->mariadb_to_cassandra(&cass_key, &cass_key_len))
  {
    /* We get here when making lookups like uuid_column='not-an-uuid' */
    DBUG_RETURN(HA_ERR_KEY_NOT_FOUND);
  }

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
  
  if (!doing_insert_batch)
    se->clear_insert_buffer();

  old_map= dbug_tmp_use_all_columns(table, table->read_set);
  
  insert_lineno++;

  /* Convert the key */
  char *cass_key;
  int cass_key_len;
  if (rowkey_converter->mariadb_to_cassandra(&cass_key, &cass_key_len))
  {
    my_error(ER_WARN_DATA_OUT_OF_RANGE, MYF(0),
             rowkey_converter->field->field_name, insert_lineno);
    dbug_tmp_restore_column_map(table->read_set, old_map);
    DBUG_RETURN(HA_ERR_AUTOINC_ERANGE);
  }
  se->start_row_insert(cass_key, cass_key_len);

  /* Convert other fields */
  for (uint i= 1; i < table->s->fields; i++)
  {
    char *cass_data;
    int cass_data_len;
    if (field_converters[i]->mariadb_to_cassandra(&cass_data, &cass_data_len))
    {
      my_error(ER_WARN_DATA_OUT_OF_RANGE, MYF(0),
               field_converters[i]->field->field_name, insert_lineno);
      dbug_tmp_restore_column_map(table->read_set, old_map);
      DBUG_RETURN(HA_ERR_AUTOINC_ERANGE);
    }
    se->add_insert_column(field_converters[i]->field->field_name, 
                          cass_data, cass_data_len);
  }

  dbug_tmp_restore_column_map(table->read_set, old_map);
  
  bool res;
  
  if (doing_insert_batch)
  {
    res= 0;
    if (++insert_rows_batched >= THDVAR(table->in_use, insert_batch_size))
    {
      res= se->do_insert();
      insert_rows_batched= 0;
    }
  }
  else
    res= se->do_insert();

  if (res)
    my_error(ER_INTERNAL_ERROR, MYF(0), se->error_str());
  
  DBUG_RETURN(res? HA_ERR_INTERNAL_ERROR: 0);
}


void ha_cassandra::start_bulk_insert(ha_rows rows)
{
  doing_insert_batch= true;
  insert_rows_batched= 0;

  se->clear_insert_buffer();
}


int ha_cassandra::end_bulk_insert()
{
  DBUG_ENTER("ha_cassandra::end_bulk_insert");
  
  /* Flush out the insert buffer */
  doing_insert_batch= false;
  bool bres= se->do_insert();
  se->clear_insert_buffer();

  DBUG_RETURN(bres? HA_ERR_INTERNAL_ERROR: 0);
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

  se->read_batch_size= THDVAR(table->in_use, rnd_batch_size);
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


int ha_cassandra::reset()
{
  doing_insert_batch= false;
  insert_lineno= 0;
  return 0;
}

/////////////////////////////////////////////////////////////////////////////
// MRR implementation
/////////////////////////////////////////////////////////////////////////////


/*
 - The key can be only primary key
  - allow equality-ranges only.
  - anything else?
*/
ha_rows ha_cassandra::multi_range_read_info_const(uint keyno, RANGE_SEQ_IF *seq,
                                                  void *seq_init_param, 
                                                  uint n_ranges, uint *bufsz,
                                                  uint *flags, COST_VECT *cost)
{
  /* No support for const ranges so far */
  return HA_POS_ERROR;
}


ha_rows ha_cassandra::multi_range_read_info(uint keyno, uint n_ranges, uint keys,
                              uint key_parts, uint *bufsz, 
                              uint *flags, COST_VECT *cost)
{
  /* Can only be equality lookups on the primary key... */
  // TODO anything else?
  *flags &= ~HA_MRR_USE_DEFAULT_IMPL;
  *flags |= HA_MRR_NO_ASSOCIATION;

  return 10;
}


int ha_cassandra::multi_range_read_init(RANGE_SEQ_IF *seq, void *seq_init_param,
                          uint n_ranges, uint mode, HANDLER_BUFFER *buf)
{
  int res;
  mrr_iter= seq->init(seq_init_param, n_ranges, mode);
  mrr_funcs= *seq;
  res= mrr_start_read();
  return (res? HA_ERR_INTERNAL_ERROR: 0);
}


bool ha_cassandra::mrr_start_read()
{
  uint key_len;

  my_bitmap_map *old_map;
  old_map= dbug_tmp_use_all_columns(table, table->read_set);
  
  se->new_lookup_keys();

  while (!(source_exhausted= mrr_funcs.next(mrr_iter, &mrr_cur_range)))
  {
    char *cass_key;
    int cass_key_len;
    
    DBUG_ASSERT(mrr_cur_range.range_flag & EQ_RANGE);

    uchar *key= (uchar*)mrr_cur_range.start_key.key;
    key_len= mrr_cur_range.start_key.length;
    //key_len= calculate_key_len(table, active_index, key, keypart_map); // NEED THIS??
    store_key_image_to_rec(table->field[0], (uchar*)key, key_len);

    rowkey_converter->mariadb_to_cassandra(&cass_key, &cass_key_len);
    
    // Primitive buffer control
    if (se->add_lookup_key(cass_key, cass_key_len) > 
        THDVAR(table->in_use, multiget_batch_size))
      break;
  }

  dbug_tmp_restore_column_map(table->read_set, old_map);

  return se->multiget_slice();
}


int ha_cassandra::multi_range_read_next(range_id_t *range_info)
{
  int res;
  while(1)
  {
    if (!se->get_next_multiget_row())
    {
      read_cassandra_columns(true);
      res= 0;
      break;
    }
    else 
    {
      if (source_exhausted)
      {
        res= HA_ERR_END_OF_FILE;
        break;
      }
      else
      {
        if (mrr_start_read())
        {
          res= HA_ERR_INTERNAL_ERROR;
          break;
        }
      }
    }
    /* 
      We get here if we've refilled the buffer and done another read. Try
      reading from results again
    */
  }
  return res;
}


int ha_cassandra::multi_range_read_explain_info(uint mrr_mode, char *str, size_t size)
{
  const char *mrr_str= "multiget_slice";

  if (!(mrr_mode & HA_MRR_USE_DEFAULT_IMPL))
  {
    uint mrr_str_len= strlen(mrr_str);
    uint copy_len= min(mrr_str_len, size);
    memcpy(str, mrr_str, size);
    return copy_len;
  }
  return 0;
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

static int show_cassandra_vars(THD *thd, SHOW_VAR *var, char *buff)
{
  //innodb_export_status();
  cassandra_counters_copy= cassandra_counters; 

  var->type= SHOW_ARRAY;
  var->value= (char *) &cassandra_status_variables;
  return 0;
}


struct st_mysql_storage_engine cassandra_storage_engine=
{ MYSQL_HANDLERTON_INTERFACE_VERSION };

static struct st_mysql_show_var func_status[]=
{
  {"Cassandra",  (char *)show_cassandra_vars, SHOW_FUNC},
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
