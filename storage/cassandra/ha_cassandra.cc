/*
   Copyright (c) 2012, Monty Program Ab

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

#ifdef USE_PRAGMA_IMPLEMENTATION
#pragma implementation        // gcc: Class implementation
#endif

#include <my_config.h>
#include <mysql/plugin.h>
#include "ha_cassandra.h"
#include "sql_class.h"

#define DYNCOL_USUAL 20
#define DYNCOL_DELTA 100
#define DYNCOL_USUAL_REC 1024
#define DYNCOL_DELTA_REC 1024

static handler *cassandra_create_handler(handlerton *hton,
                                       TABLE_SHARE *table,
                                       MEM_ROOT *mem_root);

extern int dynamic_column_error_message(enum_dyncol_func_result rc);

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
  const char *thrift_host;
  int         thrift_port;
  const char *keyspace;
  const char *column_family;
};


ha_create_table_option cassandra_table_option_list[]=
{
  /*
    one option that takes an arbitrary string
  */
  HA_TOPTION_STRING("thrift_host", thrift_host),
  HA_TOPTION_NUMBER("thrift_port", thrift_port, 9160, 1, 65535, 0),
  HA_TOPTION_STRING("keyspace", keyspace),
  HA_TOPTION_STRING("column_family", column_family),
  HA_TOPTION_END
};

/**
  Structure for CREATE TABLE options (field options).
*/

struct ha_field_option_struct
{
  bool dyncol_field;
};

ha_create_table_option cassandra_field_option_list[]=
{
  /*
    Collect all other columns as dynamic here,
    the valid values are YES/NO, ON/OFF, 1/0.
    The default is 0, that is false, no, off.
  */
  HA_FOPTION_BOOL("DYNAMIC_COLUMN_STORAGE", dyncol_field, 0),
  HA_FOPTION_END
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

static MYSQL_THDVAR_ULONG(failure_retries, PLUGIN_VAR_RQCMDARG,
  "Number of times to retry Cassandra calls that failed due to timeouts or "
  "network communication problems. The default, 0, means not to retry.",
  NULL, NULL, /*default*/ 3, /*min*/ 1, /*max*/ 1024*1024*1024, 0);

/* These match values in enum_cassandra_consistency_level */
const char *cassandra_consistency_level[] =
{
  "ONE",
  "QUORUM",
  "LOCAL_QUORUM",
  "EACH_QUORUM",
  "ALL",
  "ANY",
  "TWO",
  "THREE",
   NullS
};

TYPELIB cassandra_consistency_level_typelib= {
  array_elements(cassandra_consistency_level) - 1, "",
  cassandra_consistency_level, NULL
};


static MYSQL_THDVAR_ENUM(write_consistency, PLUGIN_VAR_RQCMDARG,
  "Cassandra consistency level to use for write operations", NULL, NULL,
  ONE, &cassandra_consistency_level_typelib);

static MYSQL_THDVAR_ENUM(read_consistency, PLUGIN_VAR_RQCMDARG,
  "Cassandra consistency level to use for read operations", NULL, NULL,
  ONE, &cassandra_consistency_level_typelib);


mysql_mutex_t cassandra_default_host_lock;
static char* cassandra_default_thrift_host = NULL;
static char cassandra_default_host_buf[256]="";

static void
cassandra_default_thrift_host_update(THD *thd,
                                     struct st_mysql_sys_var* var,
                                     void* var_ptr, /*!< out: where the
                                                    formal string goes */
                                     const void* save) /*!< in: immediate result
                                                       from check function */
{
  const char *new_host= *((char**)save);
  const size_t max_len= sizeof(cassandra_default_host_buf);

  mysql_mutex_lock(&cassandra_default_host_lock);

  if (new_host)
  {
    strncpy(cassandra_default_host_buf, new_host, max_len-1);
    cassandra_default_host_buf[max_len-1]= 0;
    cassandra_default_thrift_host= cassandra_default_host_buf;
  }
  else
  {
    cassandra_default_host_buf[0]= 0;
    cassandra_default_thrift_host= NULL;
  }

  *((const char**)var_ptr)= cassandra_default_thrift_host;

  mysql_mutex_unlock(&cassandra_default_host_lock);
}


static MYSQL_SYSVAR_STR(default_thrift_host, cassandra_default_thrift_host,
                        PLUGIN_VAR_RQCMDARG,
                        "Default host for Cassandra thrift connections",
                        /*check*/NULL,
                        cassandra_default_thrift_host_update,
                        /*default*/NULL);

static struct st_mysql_sys_var* cassandra_system_variables[]= {
  MYSQL_SYSVAR(insert_batch_size),
  MYSQL_SYSVAR(multiget_batch_size),
  MYSQL_SYSVAR(rnd_batch_size),

  MYSQL_SYSVAR(default_thrift_host),
  MYSQL_SYSVAR(write_consistency),
  MYSQL_SYSVAR(read_consistency),
  MYSQL_SYSVAR(failure_retries),
  NULL
};

Cassandra_status_vars cassandra_counters;

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
  cassandra_hton->field_options= cassandra_field_option_list;

  mysql_mutex_init(0 /* no instrumentation */,
                   &cassandra_default_host_lock, MY_MUTEX_INIT_FAST);

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
  mysql_mutex_destroy(&cassandra_default_host_lock);
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
   se(NULL), field_converters(NULL),
   special_type_field_converters(NULL),
   special_type_field_names(NULL), n_special_type_fields(0),
   rowkey_converter(NULL),
   dyncol_field(0), dyncol_set(0)
{}


int ha_cassandra::connect_and_check_options(TABLE *table_arg)
{
  ha_table_option_struct *options= table_arg->s->option_struct;
  int res;
  DBUG_ENTER("ha_cassandra::connect_and_check_options");

  if ((res= check_field_options(table_arg->s->field)) ||
      (res= check_table_options(options)))
    DBUG_RETURN(res);

  se= create_cassandra_se();
  se->set_column_family(options->column_family);
  const char *thrift_host= options->thrift_host? options->thrift_host:
                           cassandra_default_thrift_host;
  if (se->connect(thrift_host, options->thrift_port, options->keyspace))
  {
    my_error(ER_CONNECT_TO_FOREIGN_DATA_SOURCE, MYF(0), se->error_str());
    DBUG_RETURN(HA_ERR_NO_CONNECTION);
  }

  if (setup_field_converters(table_arg->field, table_arg->s->fields))
  {
    DBUG_RETURN(HA_ERR_NO_CONNECTION);
  }

  DBUG_RETURN(0);
}


int ha_cassandra::check_field_options(Field **fields)
{
  Field **field;
  uint i;
  DBUG_ENTER("ha_cassandra::check_field_options");
  for (field= fields, i= 0; *field; field++, i++)
  {
    ha_field_option_struct *field_options= (*field)->option_struct;
    if (field_options && field_options->dyncol_field)
    {
      if (dyncol_set || (*field)->type() != MYSQL_TYPE_BLOB)
      {
         my_error(ER_WRONG_FIELD_SPEC, MYF(0), (*field)->field_name);
         DBUG_RETURN(HA_WRONG_CREATE_OPTION);
      }
      dyncol_set= 1;
      dyncol_field= i;
      bzero(&dynamic_values, sizeof(dynamic_values));
      bzero(&dynamic_names, sizeof(dynamic_names));
      bzero(&dynamic_rec, sizeof(dynamic_rec));
    }
  }
  DBUG_RETURN(0);
}


int ha_cassandra::open(const char *name, int mode, uint test_if_locked)
{
  DBUG_ENTER("ha_cassandra::open");

  if (!(share = get_share(name, table)))
    DBUG_RETURN(1);
  thr_lock_data_init(&share->lock,&lock,NULL);

  DBUG_ASSERT(!se);
  /*
    Don't do the following on open: it prevents SHOW CREATE TABLE when the server
    has gone away.
  */
  /*
  int res;
  if ((res= connect_and_check_options(table)))
  {
    DBUG_RETURN(res);
  }
  */

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


int ha_cassandra::check_table_options(ha_table_option_struct *options)
{
  if (!options->thrift_host && (!cassandra_default_thrift_host ||
                                !cassandra_default_thrift_host[0]))
  {
    my_error(ER_CONNECT_TO_FOREIGN_DATA_SOURCE, MYF(0),
             "thrift_host table option must be specified, or "
             "@@cassandra_default_thrift_host must be set");
    return HA_WRONG_CREATE_OPTION;
  }

  if (!options->keyspace || !options->column_family)
  {
    my_error(ER_CONNECT_TO_FOREIGN_DATA_SOURCE, MYF(0),
             "keyspace and column_family table options must be specified");
    return HA_WRONG_CREATE_OPTION;
  }
  return 0;
}


/**
  @brief
  create() is called to create a table. The variable name will have the name
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
  int res;
  DBUG_ENTER("ha_cassandra::create");

  if (table_arg->s->keys != 1 || table_arg->s->primary_key !=0 ||
      table_arg->key_info[0].user_defined_key_parts != 1 ||
      table_arg->key_info[0].key_part[0].fieldnr != 1)
  {
    my_error(ER_WRONG_COLUMN_NAME, MYF(0),
             "Table must have PRIMARY KEY defined over the first column");
    DBUG_RETURN(HA_WRONG_CREATE_OPTION);
  }

  DBUG_ASSERT(!se);
  if ((res= connect_and_check_options(table_arg)))
    DBUG_RETURN(res);

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
  virtual int cassandra_to_mariadb(const char *cass_data,
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
  int cassandra_to_mariadb(const char *cass_data, int cass_data_len)
  {
    DBUG_ASSERT(cass_data_len == sizeof(double));
    double *pdata= (double*) cass_data;
    field->store(*pdata);
    return 0;
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
  int cassandra_to_mariadb(const char *cass_data, int cass_data_len)
  {
    DBUG_ASSERT(cass_data_len == sizeof(float));
    float *pdata= (float*) cass_data;
    field->store(*pdata);
    return 0;
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
  int cassandra_to_mariadb(const char *cass_data, int cass_data_len)
  {
    longlong tmp;
    DBUG_ASSERT(cass_data_len == sizeof(longlong));
    if (flip)
      flip64(cass_data, (char*)&tmp);
    else
      memcpy(&tmp, cass_data, sizeof(longlong));
    field->store(tmp);
    return 0;
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
  int cassandra_to_mariadb(const char *cass_data, int cass_data_len)
  {
    DBUG_ASSERT(cass_data_len == 1);
    field->store(cass_data[0]);
    return 0;
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
  int cassandra_to_mariadb(const char *cass_data, int cass_data_len)
  {
    int32_t tmp;
    DBUG_ASSERT(cass_data_len == sizeof(int32_t));
    flip32(cass_data, (char*)&tmp);
    field->store(tmp);
    return 0;
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
  size_t max_length;
public:
  int cassandra_to_mariadb(const char *cass_data, int cass_data_len)
  {
    if ((size_t)cass_data_len > max_length)
      return 1;
    field->store(cass_data, cass_data_len,field->charset());
    return 0;
  }

  bool mariadb_to_cassandra(char **cass_data, int *cass_data_len)
  {
    String *pstr= field->val_str(&buf);
    *cass_data= (char*)pstr->ptr();
    *cass_data_len= pstr->length();
    return false;
  }
  StringCopyConverter(size_t max_length_arg) : max_length(max_length_arg) {}
  ~StringCopyConverter(){}
};


class TimestampDataConverter : public ColumnDataConverter
{
  int64_t buf;
public:
  int cassandra_to_mariadb(const char *cass_data, int cass_data_len)
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
    return 0;
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

static void convert_uuid2string(char *str, const char *cass_data)
{
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
}

static bool convert_string2uuid(char *buf, const char *str)
{
  int lower, upper;
  for (uint i= 0; i < 16; i++)
  {
    if ((upper= convert_hex_digit(str[0])) == -1 ||
        (lower= convert_hex_digit(str[1])) == -1)
    {
      return true;
    }
    buf[i]= lower | (upper << 4);
    str += 2;
    if (i == 3 || i == 5 || i == 7 || i == 9)
    {
      if (str[0] != '-')
        return true;
      str++;
    }
  }
  return false;
}


class UuidDataConverter : public ColumnDataConverter
{
  char buf[16]; /* Binary UUID representation */
  String str_buf;
public:
  int cassandra_to_mariadb(const char *cass_data, int cass_data_len)
  {
    DBUG_ASSERT(cass_data_len==16);
    char str[37];
    convert_uuid2string(str, cass_data);
    field->store(str, 36,field->charset());
    return 0;
  }

  bool mariadb_to_cassandra(char **cass_data, int *cass_data_len)
  {
    String *uuid_str= field->val_str(&str_buf);

    if (uuid_str->length() != 36)
      return true;

    if (convert_string2uuid(buf, (char*)uuid_str->c_ptr()))
      return true;
    *cass_data= buf;
    *cass_data_len= 16;
    return false;
  }
  ~UuidDataConverter(){}
};

/**
  Converting dynamic columns types to/from casandra types
*/


/**
  Check and initialize (if it is needed) string MEM_ROOT
*/
static void alloc_strings_memroot(MEM_ROOT *mem_root)
{
  if (!alloc_root_inited(mem_root))
  {
    /*
      The mem_root used to allocate UUID (of length 36 + \0) so make
      appropriate allocated size
    */
    init_alloc_root(mem_root,
                    (36 + 1 + ALIGN_SIZE(sizeof(USED_MEM))) * 10 +
                    ALLOC_ROOT_MIN_BLOCK_SIZE,
                    (36 + 1 + ALIGN_SIZE(sizeof(USED_MEM))) * 10 +
                    ALLOC_ROOT_MIN_BLOCK_SIZE, MYF(MY_THREAD_SPECIFIC));
  }
}

static void free_strings_memroot(MEM_ROOT *mem_root)
{
  if (alloc_root_inited(mem_root))
    free_root(mem_root, MYF(0));
}

bool cassandra_to_dyncol_intLong(const char *cass_data,
                                 int cass_data_len __attribute__((unused)),
                                 DYNAMIC_COLUMN_VALUE *value,
                                 MEM_ROOT *mem_root __attribute__((unused)))
{
  value->type= DYN_COL_INT;
#ifdef WORDS_BIGENDIAN
  value->x.long_value= (longlong)*cass_data;
#else
  flip64(cass_data, (char *)&value->x.long_value);
#endif
  return 0;
}

bool dyncol_to_cassandraLong(DYNAMIC_COLUMN_VALUE *value,
                             char **cass_data, int *cass_data_len,
                             void* buff, void **freemem)
{
  longlong *tmp= (longlong *) buff;
  enum enum_dyncol_func_result rc=
    mariadb_dyncol_val_long(tmp, value);
  if (rc < 0)
    return true;
  *cass_data_len= sizeof(longlong);
#ifdef WORDS_BIGENDIAN
  *cass_data= (char *)buff;
#else
  flip64((char *)buff, (char *)buff + sizeof(longlong));
  *cass_data= (char *)buff + sizeof(longlong);
#endif
  *freemem= NULL;
  return false;
}

bool cassandra_to_dyncol_intInt32(const char *cass_data,
                                  int cass_data_len __attribute__((unused)),
                                  DYNAMIC_COLUMN_VALUE *value,
                                  MEM_ROOT *mem_root __attribute__((unused)))
{
  int32 tmp;
  value->type= DYN_COL_INT;
#ifdef WORDS_BIGENDIAN
  tmp= *((int32 *)cass_data);
#else
  flip32(cass_data, (char *)&tmp);
#endif
  value->x.long_value= tmp;
  return 0;
}


bool dyncol_to_cassandraInt32(DYNAMIC_COLUMN_VALUE *value,
                              char **cass_data, int *cass_data_len,
                              void* buff, void **freemem)
{
  longlong *tmp= (longlong *) ((char *)buff + sizeof(longlong));
  enum enum_dyncol_func_result rc=
    mariadb_dyncol_val_long(tmp, value);
  if (rc < 0)
    return true;
  *cass_data_len= sizeof(int32);
  *cass_data= (char *)buff;
#ifdef WORDS_BIGENDIAN
  *((int32 *) buff) = (int32) *tmp;
#else
  {
    int32 tmp2= (int32) *tmp;
    flip32((char *)&tmp2, (char *)buff);
  }
#endif
  *freemem= NULL;
  return false;
}


bool cassandra_to_dyncol_intCounter(const char *cass_data,
                                    int cass_data_len __attribute__((unused)),
                                    DYNAMIC_COLUMN_VALUE *value,
                                    MEM_ROOT *mem_root __attribute__((unused)))
{
  value->type= DYN_COL_INT;
  value->x.long_value= *((longlong *)cass_data);
  return 0;
}


bool dyncol_to_cassandraCounter(DYNAMIC_COLUMN_VALUE *value,
                                char **cass_data, int *cass_data_len,
                                void* buff, void **freemem)
{
  longlong *tmp= (longlong *)buff;
  enum enum_dyncol_func_result rc=
    mariadb_dyncol_val_long(tmp, value);
  if (rc < 0)
    return true;
  *cass_data_len= sizeof(longlong);
  *cass_data= (char *)buff;
  *freemem= NULL;
  return false;
}

bool cassandra_to_dyncol_doubleFloat(const char *cass_data,
                                     int cass_data_len __attribute__((unused)),
                                     DYNAMIC_COLUMN_VALUE *value,
                                     MEM_ROOT *mem_root __attribute__((unused)))
{
  value->type= DYN_COL_DOUBLE;
  value->x.double_value= *((float *)cass_data);
  return 0;
}

bool dyncol_to_cassandraFloat(DYNAMIC_COLUMN_VALUE *value,
                              char **cass_data, int *cass_data_len,
                              void* buff, void **freemem)
{
  double tmp;
  enum enum_dyncol_func_result rc=
    mariadb_dyncol_val_double(&tmp, value);
  if (rc < 0)
    return true;
  *((float *)buff)= (float) tmp;
  *cass_data_len= sizeof(float);
  *cass_data= (char *)buff;
  *freemem= NULL;
  return false;
}

bool cassandra_to_dyncol_doubleDouble(const char *cass_data,
                                      int cass_data_len __attribute__((unused)),
                                      DYNAMIC_COLUMN_VALUE *value,
                                      MEM_ROOT *mem_root
                                      __attribute__((unused)))
{
  value->type= DYN_COL_DOUBLE;
  value->x.double_value= *((double *)cass_data);
  return 0;
}

bool dyncol_to_cassandraDouble(DYNAMIC_COLUMN_VALUE *value,
                               char **cass_data, int *cass_data_len,
                               void* buff, void **freemem)
{
  double *tmp= (double *)buff;
  enum enum_dyncol_func_result rc=
    mariadb_dyncol_val_double(tmp, value);
  if (rc < 0)
    return true;
  *cass_data_len= sizeof(double);
  *cass_data= (char *)buff;
  *freemem= NULL;
  return false;
}

bool cassandra_to_dyncol_strStr(const char *cass_data,
                                int cass_data_len,
                                DYNAMIC_COLUMN_VALUE *value,
                                CHARSET_INFO *cs)
{
  value->type= DYN_COL_STRING;
  value->x.string.charset= cs;
  value->x.string.value.str= (char *)cass_data;
  value->x.string.value.length= cass_data_len;
  return 0;
}

bool dyncol_to_cassandraStr(DYNAMIC_COLUMN_VALUE *value,
                            char **cass_data, int *cass_data_len,
                            void* buff, void **freemem, CHARSET_INFO *cs)
{
  DYNAMIC_STRING tmp;
  if (init_dynamic_string(&tmp, NULL, 1024, 1024))
    return 1;
  enum enum_dyncol_func_result rc=
    mariadb_dyncol_val_str(&tmp, value, cs, '\0');
  if (rc < 0)
  {
    dynstr_free(&tmp);
    return 1;
  }
  *cass_data_len= tmp.length;
  *(cass_data)= tmp.str;
  *freemem= tmp.str;
  return 0;
}

bool cassandra_to_dyncol_strBytes(const char *cass_data,
                                  int cass_data_len,
                                  DYNAMIC_COLUMN_VALUE *value,
                                  MEM_ROOT *mem_root __attribute__((unused)))
{
  return cassandra_to_dyncol_strStr(cass_data, cass_data_len, value,
                                    &my_charset_bin);
}

bool dyncol_to_cassandraBytes(DYNAMIC_COLUMN_VALUE *value,
                              char **cass_data, int *cass_data_len,
                              void* buff, void **freemem)
{
  return dyncol_to_cassandraStr(value, cass_data, cass_data_len,
                                buff, freemem, &my_charset_bin);
}

bool cassandra_to_dyncol_strAscii(const char *cass_data,
                                  int cass_data_len,
                                  DYNAMIC_COLUMN_VALUE *value,
                                  MEM_ROOT *mem_root __attribute__((unused)))
{
  return cassandra_to_dyncol_strStr(cass_data, cass_data_len, value,
                                    &my_charset_latin1_bin);
}

bool dyncol_to_cassandraAscii(DYNAMIC_COLUMN_VALUE *value,
                              char **cass_data, int *cass_data_len,
                              void* buff, void **freemem)
{
  return dyncol_to_cassandraStr(value, cass_data, cass_data_len,
                                buff, freemem, &my_charset_latin1_bin);
}

bool cassandra_to_dyncol_strUTF8(const char *cass_data,
                                 int cass_data_len,
                                 DYNAMIC_COLUMN_VALUE *value,
                                 MEM_ROOT *mem_root __attribute__((unused)))
{
  return cassandra_to_dyncol_strStr(cass_data, cass_data_len, value,
                                    &my_charset_utf8_unicode_ci);
}

bool dyncol_to_cassandraUTF8(DYNAMIC_COLUMN_VALUE *value,
                             char **cass_data, int *cass_data_len,
                             void* buff, void **freemem)
{
  return dyncol_to_cassandraStr(value, cass_data, cass_data_len,
                                buff, freemem, &my_charset_utf8_unicode_ci);
}

bool cassandra_to_dyncol_strUUID(const char *cass_data,
                                 int cass_data_len,
                                 DYNAMIC_COLUMN_VALUE *value,
                                 MEM_ROOT *mem_root)
{
  value->type= DYN_COL_STRING;
  value->x.string.charset= &my_charset_bin;
  alloc_strings_memroot(mem_root);
  value->x.string.value.str= (char *)alloc_root(mem_root, 37);
  if (!value->x.string.value.str)
  {
    value->x.string.value.length= 0;
    return 1;
  }
  convert_uuid2string(value->x.string.value.str, cass_data);
  value->x.string.value.length= 36;
  return 0;
}

bool dyncol_to_cassandraUUID(DYNAMIC_COLUMN_VALUE *value,
                             char **cass_data, int *cass_data_len,
                             void* buff, void **freemem)
{
  DYNAMIC_STRING tmp;
  if (init_dynamic_string(&tmp, NULL, 1024, 1024))
    return true;
  enum enum_dyncol_func_result rc=
    mariadb_dyncol_val_str(&tmp, value, &my_charset_latin1_bin, '\0');
  if (rc < 0 || tmp.length != 36 || convert_string2uuid((char *)buff, tmp.str))
  {
    dynstr_free(&tmp);
    return true;
  }

  *cass_data_len= tmp.length;
  *(cass_data)= tmp.str;
  *freemem= tmp.str;
  return 0;
}

bool cassandra_to_dyncol_intBool(const char *cass_data,
                                 int cass_data_len,
                                 DYNAMIC_COLUMN_VALUE *value,
                                 MEM_ROOT *mem_root __attribute__((unused)))
{
  value->type= DYN_COL_INT;
  value->x.long_value= (cass_data[0] ? 1 : 0);
  return 0;
}

bool dyncol_to_cassandraBool(DYNAMIC_COLUMN_VALUE *value,
                             char **cass_data, int *cass_data_len,
                             void* buff, void **freemem)
{
  longlong tmp;
  enum enum_dyncol_func_result rc=
    mariadb_dyncol_val_long(&tmp, value);
  if (rc < 0)
    return true;
  ((char *)buff)[0]= (tmp ? 1 : 0);
  *cass_data_len= 1;
  *(cass_data)= (char *)buff;
  *freemem= 0;
  return 0;
}


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

/* VARINTs are stored as big-endian big numbers. */
const char * const validator_varint= "org.apache.cassandra.db.marshal.IntegerType";
const char * const validator_decimal= "org.apache.cassandra.db.marshal.DecimalType";


static CASSANDRA_TYPE_DEF cassandra_types[]=
{
  {
    validator_bigint,
    &cassandra_to_dyncol_intLong,
    &dyncol_to_cassandraLong
  },
  {
    validator_int,
    &cassandra_to_dyncol_intInt32,
    &dyncol_to_cassandraInt32
  },
  {
    validator_counter,
    cassandra_to_dyncol_intCounter,
    &dyncol_to_cassandraCounter
  },
  {
    validator_float,
    &cassandra_to_dyncol_doubleFloat,
    &dyncol_to_cassandraFloat
  },
  {
    validator_double,
    &cassandra_to_dyncol_doubleDouble,
    &dyncol_to_cassandraDouble
  },
  {
    validator_blob,
    &cassandra_to_dyncol_strBytes,
    &dyncol_to_cassandraBytes
  },
  {
    validator_ascii,
    &cassandra_to_dyncol_strAscii,
    &dyncol_to_cassandraAscii
  },
  {
    validator_text,
    &cassandra_to_dyncol_strUTF8,
    &dyncol_to_cassandraUTF8
  },
  {
    validator_timestamp,
    &cassandra_to_dyncol_intLong,
    &dyncol_to_cassandraLong
  },
  {
    validator_uuid,
    &cassandra_to_dyncol_strUUID,
    &dyncol_to_cassandraUUID
  },
  {
    validator_boolean,
    &cassandra_to_dyncol_intBool,
    &dyncol_to_cassandraBool
  },
  {
    validator_varint,
    &cassandra_to_dyncol_strBytes,
    &dyncol_to_cassandraBytes
  },
  {
    validator_decimal,
    &cassandra_to_dyncol_strBytes,
    &dyncol_to_cassandraBytes
  }
};

CASSANDRA_TYPE get_cassandra_type(const char *validator)
{
  CASSANDRA_TYPE rc;
  switch(validator[32])
  {
  case 'L':
    rc= CT_BIGINT;
    break;
  case 'I':
    rc= (validator[35] == '3' ? CT_INT : CT_VARINT);
    rc= CT_INT;
    break;
  case 'C':
    rc= CT_COUNTER;
    break;
  case 'F':
    rc= CT_FLOAT;
    break;
  case 'D':
    switch (validator[33])
    {
    case 'o':
      rc= CT_DOUBLE;
      break;
    case 'a':
      rc= CT_TIMESTAMP;
      break;
    case 'e':
      rc= CT_DECIMAL;
      break;
    default:
      rc= CT_BLOB;
      break;
    }
    break;
  case 'B':
    rc= (validator[33] == 'o' ? CT_BOOLEAN : CT_BLOB);
    break;
  case 'A':
    rc= CT_ASCII;
    break;
  case 'U':
    rc= (validator[33] == 'T' ? CT_TEXT : CT_UUID);
    break;
  default:
    rc= CT_BLOB;
  }
  DBUG_ASSERT(strcmp(cassandra_types[rc].name, validator) == 0);
  return rc;
}

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
          !strcmp(validator_name, validator_timestamp) ||
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
    case MYSQL_TYPE_VAR_STRING:
    case MYSQL_TYPE_VARCHAR:
    case MYSQL_TYPE_BLOB:
    {
      /*
        Cassandra's "varint" type is a binary-encoded arbitary-length
        big-endian number.
        - It can be mapped to VARBINARY(N), with sufficiently big N.
        - If the value does not fit into N bytes, it is an error. We should not
          truncate it, because that is just as good as returning garbage.
        - varint should not be mapped to BINARY(N), because BINARY(N) values
          are zero-padded, which will work as multiplying the value by
          2^k for some value of k.
      */
      if (field->type() == MYSQL_TYPE_VARCHAR &&
          field->binary() &&
          (!strcmp(validator_name, validator_varint) ||
           !strcmp(validator_name, validator_decimal)))
      {
        res= new StringCopyConverter(field->field_length);
        break;
      }

      if (!strcmp(validator_name, validator_blob) ||
          !strcmp(validator_name, validator_ascii) ||
          !strcmp(validator_name, validator_text))
      {
        res= new StringCopyConverter((size_t)-1);
      }
      break;
    }
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
  size_t ddl_fields= se->get_ddl_size();
  const char *default_type= se->get_default_validator();
  uint max_non_default_fields;
  DBUG_ENTER("ha_cassandra::setup_field_converters");
  DBUG_ASSERT(default_type);

  DBUG_ASSERT(!field_converters);
  DBUG_ASSERT(dyncol_set == 0 || dyncol_set == 1);

  /*
    We always should take into account that in case of using dynamic columns
    sql description contain one field which does not described in
    Cassandra DDL also key field is described separately. So that
    is why we use "n_fields - dyncol_set - 1" or "ddl_fields + 2".
  */
  max_non_default_fields= ddl_fields + 2 - n_fields;
  if (ddl_fields < (n_fields - dyncol_set - 1))
  {
    se->print_error("Some of SQL fields were not mapped to Cassandra's fields");
    my_error(ER_INTERNAL_ERROR, MYF(0), se->error_str());
    DBUG_RETURN(true);
  }

  /* allocate memory in one chunk */
  size_t memsize= sizeof(ColumnDataConverter*) * n_fields +
    (sizeof(LEX_STRING) + sizeof(CASSANDRA_TYPE_DEF))*
    (dyncol_set ? max_non_default_fields : 0);
  if (!(field_converters= (ColumnDataConverter**)my_malloc(memsize, MYF(0))))
    DBUG_RETURN(true);
  bzero(field_converters, memsize);
  n_field_converters= n_fields;

  if (dyncol_set)
  {
    special_type_field_converters=
      (CASSANDRA_TYPE_DEF *)(field_converters + n_fields);
    special_type_field_names=
      ((LEX_STRING*)(special_type_field_converters + max_non_default_fields));

    if (my_init_dynamic_array(&dynamic_values,
                           sizeof(DYNAMIC_COLUMN_VALUE),
                           DYNCOL_USUAL, DYNCOL_DELTA, MYF(0)))
      DBUG_RETURN(true);
    else
      if (my_init_dynamic_array(&dynamic_names,
                             sizeof(LEX_STRING),
                             DYNCOL_USUAL, DYNCOL_DELTA,MYF(0)))
      {
        delete_dynamic(&dynamic_values);
        DBUG_RETURN(true);
      }
      else
        if (init_dynamic_string(&dynamic_rec, NULL,
                                DYNCOL_USUAL_REC, DYNCOL_DELTA_REC))
        {
          delete_dynamic(&dynamic_values);
          delete_dynamic(&dynamic_names);
          DBUG_RETURN(true);
        }

    /* Dynamic column field has special processing */
    field_converters[dyncol_field]= NULL;

    default_type_def= cassandra_types + get_cassandra_type(default_type);
  }

  se->first_ddl_column();
  uint n_mapped= 0;
  while (!se->next_ddl_column(&col_name, &col_name_len, &col_type,
                              &col_type_len))
  {
    Field **field;
    uint i;
    /* Mapping for the 1st field is already known */
    for (field= field_arg + 1, i= 1; *field; field++, i++)
    {
      if ((!dyncol_set || dyncol_field != i) &&
          !strcmp((*field)->field_name, col_name))
      {
        n_mapped++;
        ColumnDataConverter **conv= field_converters + (*field)->field_index;
        if (!(*conv= map_field_to_validator(*field, col_type)))
        {
          se->print_error("Failed to map column %s to datatype %s",
                          (*field)->field_name, col_type);
          my_error(ER_INTERNAL_ERROR, MYF(0), se->error_str());
          DBUG_RETURN(true);
        }
        (*conv)->field= *field;
        break;
      }
    }
    if (dyncol_set && !(*field)) // is needed and not found
    {
      DBUG_PRINT("info",("Field not found: %s", col_name));
      if (strcmp(col_type, default_type))
      {
        DBUG_PRINT("info",("Field '%s' non-default type: '%s'",
                           col_name, col_type));
        special_type_field_names[n_special_type_fields].length= col_name_len;
        special_type_field_names[n_special_type_fields].str= col_name;
        special_type_field_converters[n_special_type_fields]=
          cassandra_types[get_cassandra_type(col_type)];
        n_special_type_fields++;
      }
    }
  }

  if (n_mapped != n_fields - 1 - dyncol_set)
  {
    Field *first_unmapped= NULL;
    /* Find the first field */
    for (uint i= 1; i < n_fields;i++)
    {
      if (!field_converters[i])
      {
        first_unmapped= field_arg[i];
        break;
      }
    }
    DBUG_ASSERT(first_unmapped);

    se->print_error("Field `%s` could not be mapped to any field in Cassandra",
                    first_unmapped->field_name);
    my_error(ER_INTERNAL_ERROR, MYF(0), se->error_str());
    DBUG_RETURN(true);
  }

  /*
    Setup type conversion for row_key.
  */
  se->get_rowkey_type(&col_name, &col_type);
  if (col_name && strcmp(col_name, (*field_arg)->field_name))
  {
    se->print_error("PRIMARY KEY column must match Cassandra's name '%s'",
                    col_name);
    my_error(ER_INTERNAL_ERROR, MYF(0), se->error_str());
    DBUG_RETURN(true);
  }
  if (!col_name && strcmp("rowkey", (*field_arg)->field_name))
  {
    se->print_error("target column family has no key_alias defined, "
                    "PRIMARY KEY column must be named 'rowkey'");
    my_error(ER_INTERNAL_ERROR, MYF(0), se->error_str());
    DBUG_RETURN(true);
  }

  if (col_type != NULL)
  {
    if (!(rowkey_converter= map_field_to_validator(*field_arg, col_type)))
    {
      se->print_error("Failed to map PRIMARY KEY to datatype %s", col_type);
      my_error(ER_INTERNAL_ERROR, MYF(0), se->error_str());
      DBUG_RETURN(true);
    }
    rowkey_converter->field= *field_arg;
  }
  else
  {
    se->print_error("Cassandra's rowkey has no defined datatype (todo: support this)");
    my_error(ER_INTERNAL_ERROR, MYF(0), se->error_str());
    DBUG_RETURN(true);
  }

  DBUG_RETURN(false);
}


void ha_cassandra::free_field_converters()
{
  delete rowkey_converter;
  rowkey_converter= NULL;

  if (dyncol_set)
  {
    delete_dynamic(&dynamic_values);
    delete_dynamic(&dynamic_names);
    dynstr_free(&dynamic_rec);
  }
  if (field_converters)
  {
    for (uint i=0; i < n_field_converters; i++)
      if (field_converters[i])
      {
        DBUG_ASSERT(!dyncol_set || i != dyncol_field);
        delete field_converters[i];
      }
    my_free(field_converters);
    field_converters= NULL;
  }
}


int ha_cassandra::index_init(uint idx, bool sorted)
{
  int ires;
  if (!se && (ires= connect_and_check_options(table)))
    return ires;
  return 0;
}

void store_key_image_to_rec(Field *field, uchar *ptr, uint len);

int ha_cassandra::index_read_map(uchar *buf, const uchar *key,
                                 key_part_map keypart_map,
                                 enum ha_rkey_function find_flag)
{
  int rc= 0;
  DBUG_ENTER("ha_cassandra::index_read_map");

  if (find_flag != HA_READ_KEY_EXACT)
  {
    DBUG_ASSERT(0); /* Non-equality lookups should never be done */
    DBUG_RETURN(HA_ERR_WRONG_COMMAND);
  }

  uint key_len= calculate_key_len(table, active_index, key, keypart_map);
  store_key_image_to_rec(table->field[0], (uchar*)key, key_len);

  char *cass_key;
  int cass_key_len;
  MY_BITMAP *old_map;

  old_map= dbug_tmp_use_all_columns(table, &table->read_set);

  if (rowkey_converter->mariadb_to_cassandra(&cass_key, &cass_key_len))
  {
    /* We get here when making lookups like uuid_column='not-an-uuid' */
    dbug_tmp_restore_column_map(&table->read_set, old_map);
    DBUG_RETURN(HA_ERR_KEY_NOT_FOUND);
  }

  dbug_tmp_restore_column_map(&table->read_set, old_map);

  bool found;
  if (se->get_slice(cass_key, cass_key_len, &found))
  {
    my_error(ER_INTERNAL_ERROR, MYF(0), se->error_str());
    rc= HA_ERR_INTERNAL_ERROR;
  }

  /* TODO: what if we're not reading all columns?? */
  if (!found)
    rc= HA_ERR_KEY_NOT_FOUND;
  else
    rc= read_cassandra_columns(false);

  DBUG_RETURN(rc);
}


void ha_cassandra::print_conversion_error(const char *field_name,
                                          char *cass_value,
                                          int cass_value_len)
{
  char buf[32];
  char *p= cass_value;
  size_t i= 0;
  for (; (i < sizeof(buf)-1) && (p < cass_value + cass_value_len); p++)
  {
    buf[i++]= map2number[(*p >> 4) & 0xF];
    buf[i++]= map2number[*p & 0xF];
  }
  buf[i]=0;

  se->print_error("Unable to convert value for field `%s` from Cassandra's data"
                  " format. Source data is %d bytes, 0x%s%s",
                  field_name, cass_value_len, buf,
                  (i == sizeof(buf) - 1)? "..." : "");
  my_error(ER_INTERNAL_ERROR, MYF(0), se->error_str());
}



CASSANDRA_TYPE_DEF * ha_cassandra::get_cassandra_field_def(char *cass_name,
                                                           int cass_name_len)
{
  CASSANDRA_TYPE_DEF *type= default_type_def;
  for(uint i= 0; i < n_special_type_fields; i++)
  {
    if (cass_name_len == (int)special_type_field_names[i].length &&
        memcmp(cass_name, special_type_field_names[i].str,
               cass_name_len) == 0)
    {
      type= special_type_field_converters + i;
      break;
    }
  }
  return type;
}

int ha_cassandra::read_cassandra_columns(bool unpack_pk)
{
  MEM_ROOT strings_root;
  char *cass_name;
  char *cass_value;
  int cass_value_len, cass_name_len;
  Field **field;
  int res= 0;
  ulong total_name_len= 0;

  clear_alloc_root(&strings_root);
  /*
    cassandra_to_mariadb() calls will use field->store(...) methods, which
    require that the column is in the table->write_set
  */
  MY_BITMAP *old_map;
  old_map= dbug_tmp_use_all_columns(table, &table->write_set);

  /* Start with all fields being NULL */
  for (field= table->field + 1; *field; field++)
    (*field)->set_null();

  while (!se->get_next_read_column(&cass_name, &cass_name_len,
                                   &cass_value, &cass_value_len))
  {
    // map to our column. todo: use hash or something..
    bool found= 0;
    for (field= table->field + 1; *field; field++)
    {
      uint fieldnr= (*field)->field_index;
      if ((!dyncol_set || dyncol_field != fieldnr) &&
          !strcmp((*field)->field_name, cass_name))
      {
        found= 1;
        (*field)->set_notnull();
        if (field_converters[fieldnr]->cassandra_to_mariadb(cass_value,
                                                            cass_value_len))
        {
          print_conversion_error((*field)->field_name, cass_value,
                                 cass_value_len);
          res=1;
          goto err;
        }
        break;
      }
    }
    if (dyncol_set && !found)
    {
      DYNAMIC_COLUMN_VALUE val;
      LEX_STRING nm;
      CASSANDRA_TYPE_DEF *type= get_cassandra_field_def(cass_name,
                                                        cass_name_len);
      nm.str= cass_name;
      nm.length= cass_name_len;
      if (nm.length > MAX_NAME_LENGTH)
      {
        se->print_error("Unable to convert value for field `%s`"
                        " from Cassandra's data format. Name"
                        " length exceed limit of %u: '%s'",
                        table->field[dyncol_field]->field_name,
                        (uint)MAX_NAME_LENGTH, cass_name);
        my_error(ER_INTERNAL_ERROR, MYF(0), se->error_str());
        res=1;
        goto err;
      }
      total_name_len+= cass_name_len;
      if (nm.length > MAX_TOTAL_NAME_LENGTH)
      {
        se->print_error("Unable to convert value for field `%s`"
                        " from Cassandra's data format. Sum of all names"
                        " length exceed limit of %lu",
                        table->field[dyncol_field]->field_name,
                        cass_name, (uint)MAX_TOTAL_NAME_LENGTH);
        my_error(ER_INTERNAL_ERROR, MYF(0), se->error_str());
        res=1;
        goto err;
      }

      if ((res= (*(type->cassandra_to_dynamic))(cass_value,
                                                cass_value_len, &val,
                                                &strings_root)) ||
          insert_dynamic(&dynamic_names, (uchar *) &nm) ||
          insert_dynamic(&dynamic_values, (uchar *) &val))
      {
        if (res)
        {
          print_conversion_error(cass_name, cass_value, cass_value_len);
        }
        free_strings_memroot(&strings_root);
        // EOM shouldm be already reported if happened
        res= 1;
        goto err;
      }
    }
  }

  dynamic_rec.length= 0;
  if (dyncol_set)
  {
    if (mariadb_dyncol_create_many_named(&dynamic_rec,
                                         dynamic_names.elements,
                                         (LEX_STRING *)dynamic_names.buffer,
                                         (DYNAMIC_COLUMN_VALUE *)
                                         dynamic_values.buffer,
                                         FALSE) < 0)
      dynamic_rec.length= 0;

    free_strings_memroot(&strings_root);
    dynamic_values.elements= dynamic_names.elements= 0;

    if (dynamic_rec.length == 0)
      table->field[dyncol_field]->set_null();
    else
    {
      Field_blob *blob= (Field_blob *)table->field[dyncol_field];
      blob->set_notnull();
      blob->store_length(dynamic_rec.length);
      *((char **)(((char *)blob->ptr) + blob->pack_length_no_ptr()))=
        dynamic_rec.str;
    }
  }

  if (unpack_pk)
  {
    /* Unpack rowkey to primary key */
    field= table->field;
    (*field)->set_notnull();
    se->get_read_rowkey(&cass_value, &cass_value_len);
    if (rowkey_converter->cassandra_to_mariadb(cass_value, cass_value_len))
    {
      print_conversion_error((*field)->field_name, cass_value, cass_value_len);
      res=1;
      goto err;
    }
  }

err:
  dbug_tmp_restore_column_map(&table->write_set, old_map);
  return res;
}

int ha_cassandra::read_dyncol(uint *count,
                              DYNAMIC_COLUMN_VALUE **vals,
                              LEX_STRING **names,
                              String *valcol)
{
  String *strcol;
  DYNAMIC_COLUMN col;

  enum enum_dyncol_func_result rc;
  DBUG_ENTER("ha_cassandra::read_dyncol");

  Field *field= table->field[dyncol_field];
  DBUG_ASSERT(field->type() == MYSQL_TYPE_BLOB);
  /* It is blob and it does not use buffer */
  strcol= field->val_str(NULL, valcol);
  if (field->is_null())
  {
    *count= 0;
    *names= 0;
    *vals= 0;
    DBUG_RETURN(0); // nothing to write
  }
  /*
    dynamic_column_vals only read the string so we can
    cheat here with assignment
  */
  bzero(&col, sizeof(col));
  col.str= (char *)strcol->ptr();
  col.length= strcol->length();
  if ((rc= mariadb_dyncol_unpack(&col, count, names, vals)) < 0)
  {
    dynamic_column_error_message(rc);
    DBUG_RETURN(HA_ERR_INTERNAL_ERROR);
  }
  DBUG_RETURN(0);
}

int ha_cassandra::write_dynamic_row(uint count,
                                    DYNAMIC_COLUMN_VALUE *vals,
                                    LEX_STRING *names)
{
  uint i;
  DBUG_ENTER("ha_cassandra::write_dynamic_row");
  DBUG_ASSERT(dyncol_set);


  for (i= 0; i < count; i++)
  {
    char buff[16];
    CASSANDRA_TYPE_DEF *type;
    void *freemem= NULL;
    char *cass_data;
    int cass_data_len;

    DBUG_PRINT("info", ("field %*s", (int)names[i].length, names[i].str));
    type= get_cassandra_field_def(names[i].str, (int) names[i].length);
    if ((*type->dynamic_to_cassandra)(vals +i, &cass_data, &cass_data_len,
                                      buff, &freemem))
    {
      my_error(ER_WARN_DATA_OUT_OF_RANGE, MYF(0),
               names[i].str, insert_lineno);
      DBUG_RETURN(HA_ERR_GENERIC);
    }
    se->add_insert_column(names[i].str, names[i].length,
                          cass_data, cass_data_len);
    if (freemem)
      my_free(freemem);
  }
  DBUG_RETURN(0);
}

void ha_cassandra::free_dynamic_row(DYNAMIC_COLUMN_VALUE **vals,
                                    LEX_STRING **names)
{
  mariadb_dyncol_unpack_free(*names, *vals);
  *vals= 0;
  *names= 0;
}

int ha_cassandra::write_row(uchar *buf)
{
  MY_BITMAP *old_map;
  int ires;
  DBUG_ENTER("ha_cassandra::write_row");

  if (!se && (ires= connect_and_check_options(table)))
    DBUG_RETURN(ires);

  if (!doing_insert_batch)
    se->clear_insert_buffer();

  old_map= dbug_tmp_use_all_columns(table, &table->read_set);

  insert_lineno++;

  /* Convert the key */
  char *cass_key;
  int cass_key_len;
  if (rowkey_converter->mariadb_to_cassandra(&cass_key, &cass_key_len))
  {
    my_error(ER_WARN_DATA_OUT_OF_RANGE, MYF(0),
             rowkey_converter->field->field_name, insert_lineno);
    dbug_tmp_restore_column_map(&table->read_set, old_map);
    DBUG_RETURN(HA_ERR_INTERNAL_ERROR);
  }
  se->start_row_insert(cass_key, cass_key_len);

  /* Convert other fields */
  for (uint i= 1; i < table->s->fields; i++)
  {
    char *cass_data;
    int cass_data_len;
    if (dyncol_set && dyncol_field == i)
    {
      String valcol;
      DYNAMIC_COLUMN_VALUE *vals;
      LEX_STRING *names;
      uint count;
      int rc;
      DBUG_ASSERT(field_converters[i] == NULL);
      if (!(rc= read_dyncol(&count, &vals, &names, &valcol)))
        rc= write_dynamic_row(count, vals, names);
      free_dynamic_row(&vals, &names);
      if (rc)
      {
        dbug_tmp_restore_column_map(&table->read_set, old_map);
        DBUG_RETURN(rc);
      }
    }
    else
    {
      if (field_converters[i]->mariadb_to_cassandra(&cass_data,
                                                    &cass_data_len))
      {
        my_error(ER_WARN_DATA_OUT_OF_RANGE, MYF(0),
                 field_converters[i]->field->field_name, insert_lineno);
        dbug_tmp_restore_column_map(&table->read_set, old_map);
        DBUG_RETURN(HA_ERR_INTERNAL_ERROR);
      }
      se->add_insert_column(field_converters[i]->field->field_name, 0,
                            cass_data, cass_data_len);
    }
  }

  dbug_tmp_restore_column_map(&table->read_set, old_map);

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


void ha_cassandra::start_bulk_insert(ha_rows rows, uint flags)
{
  int ires;
  if (!se && (ires= connect_and_check_options(table)))
    return;

  doing_insert_batch= true;
  insert_rows_batched= 0;

  se->clear_insert_buffer();
}


int ha_cassandra::end_bulk_insert()
{
  DBUG_ENTER("ha_cassandra::end_bulk_insert");
  
  if (!doing_insert_batch)
  {
    /* SQL layer can make end_bulk_insert call without start_bulk_insert call */
    DBUG_RETURN(0);
  }

  /* Flush out the insert buffer */
  doing_insert_batch= false;
  bool bres= se->do_insert();
  se->clear_insert_buffer();

  DBUG_RETURN(bres? HA_ERR_INTERNAL_ERROR: 0);
}


int ha_cassandra::rnd_init(bool scan)
{
  bool bres;
  int ires;
  DBUG_ENTER("ha_cassandra::rnd_init");

  if (!se && (ires= connect_and_check_options(table)))
    DBUG_RETURN(ires);

  if (!scan)
  {
    /* Prepare for rnd_pos() calls. We don't need to anything. */
    DBUG_RETURN(0);
  }

  if (dyncol_set)
  {
    se->clear_read_all_columns();
  }
  else
  {
    se->clear_read_columns();
    for (uint i= 1; i < table->s->fields; i++)
      se->add_read_column(table->field[i]->field_name);
  }

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
      rc= read_cassandra_columns(true);
  }

  DBUG_RETURN(rc);
}


int ha_cassandra::delete_all_rows()
{
  bool bres;
  int ires;
  DBUG_ENTER("ha_cassandra::delete_all_rows");

  if (!se && (ires= connect_and_check_options(table)))
    DBUG_RETURN(ires);

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
    stats.deleted= 0;
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
  if (se)
  {
    se->set_consistency_levels(THDVAR(table->in_use, read_consistency),
                               THDVAR(table->in_use, write_consistency));
    se->set_n_retries(THDVAR(table->in_use, failure_retries));
  }
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
                                                  uint *flags, Cost_estimate *cost)
{
  /* No support for const ranges so far */
  return HA_POS_ERROR;
}


ha_rows ha_cassandra::multi_range_read_info(uint keyno, uint n_ranges, uint keys,
                              uint key_parts, uint *bufsz,
                              uint *flags, Cost_estimate *cost)
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

  MY_BITMAP *old_map;
  old_map= dbug_tmp_use_all_columns(table, &table->read_set);

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
    if ((ulong) se->add_lookup_key(cass_key, cass_key_len) >
        THDVAR(table->in_use, multiget_batch_size))
      break;
  }

  dbug_tmp_restore_column_map(&table->read_set, old_map);

  return se->multiget_slice();
}


int ha_cassandra::multi_range_read_next(range_id_t *range_info)
{
  int res;
  while(1)
  {
    if (!se->get_next_multiget_row())
    {
      res= read_cassandra_columns(true);
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
    uint copy_len= MY_MIN(mrr_str_len, size);
    memcpy(str, mrr_str, size);
    return copy_len;
  }
  return 0;
}


class Column_name_enumerator_impl : public Column_name_enumerator
{
  ha_cassandra *obj;
  uint idx;
public:
  Column_name_enumerator_impl(ha_cassandra *obj_arg) : obj(obj_arg), idx(1) {}
  const char* get_next_name()
  {
    if (idx == obj->table->s->fields)
      return NULL;
    else
      return obj->table->field[idx++]->field_name;
  }
};


int ha_cassandra::update_row(const uchar *old_data, uchar *new_data)
{
  DYNAMIC_COLUMN_VALUE *oldvals, *vals;
  LEX_STRING *oldnames, *names;
  uint oldcount, count;
  String oldvalcol, valcol;
  MY_BITMAP *old_map;
  int res;
  DBUG_ENTER("ha_cassandra::update_row");
  /* Currently, it is guaranteed that new_data == table->record[0] */
  DBUG_ASSERT(new_data == table->record[0]);
  /* For now, just rewrite the full record */
  se->clear_insert_buffer();

  old_map= dbug_tmp_use_all_columns(table, &table->read_set);

  char *old_key;
  int old_key_len;
  se->get_read_rowkey(&old_key, &old_key_len);

  /* Get the key we're going to write */
  char *new_key;
  int new_key_len;
  if (rowkey_converter->mariadb_to_cassandra(&new_key, &new_key_len))
  {
    my_error(ER_WARN_DATA_OUT_OF_RANGE, MYF(0),
             rowkey_converter->field->field_name, insert_lineno);
    dbug_tmp_restore_column_map(&table->read_set, old_map);
    DBUG_RETURN(HA_ERR_INTERNAL_ERROR);
  }

  /*
    Compare it to the key we've read. For all types that Cassandra supports,
    binary byte-wise comparison can be used
  */
  bool new_primary_key;
  if (new_key_len != old_key_len || memcmp(old_key, new_key, new_key_len))
    new_primary_key= true;
  else
    new_primary_key= false;

  if (dyncol_set)
  {
    Field *field= table->field[dyncol_field];
    /* move to get old_data */
    my_ptrdiff_t diff;
    diff= (my_ptrdiff_t) (old_data - new_data);
    field->move_field_offset(diff);      // Points now at old_data
    if ((res= read_dyncol(&oldcount, &oldvals, &oldnames, &oldvalcol)))
      DBUG_RETURN(res);
    field->move_field_offset(-diff);     // back to new_data
    if ((res= read_dyncol(&count, &vals, &names, &valcol)))
    {
      free_dynamic_row(&oldvals, &oldnames);
      DBUG_RETURN(res);
    }
  }

  if (new_primary_key)
  {
    /*
      Primary key value changed. This is essentially a DELETE + INSERT.
      Add a DELETE operation into the batch
    */
    Column_name_enumerator_impl name_enumerator(this);
    se->add_row_deletion(old_key, old_key_len, &name_enumerator,
                         oldnames,
                         (dyncol_set ? oldcount : 0));
    oldcount= 0; // they will be deleted
  }

  se->start_row_insert(new_key, new_key_len);

  /* Convert other fields */
  for (uint i= 1; i < table->s->fields; i++)
  {
    char *cass_data;
    int cass_data_len;
    if (dyncol_set && dyncol_field == i)
    {
      DBUG_ASSERT(field_converters[i] == NULL);
      if ((res= write_dynamic_row(count, vals, names)))
        goto err;
    }
    else
    {
      if (field_converters[i]->mariadb_to_cassandra(&cass_data, &cass_data_len))
      {
        my_error(ER_WARN_DATA_OUT_OF_RANGE, MYF(0),
                 field_converters[i]->field->field_name, insert_lineno);
        dbug_tmp_restore_column_map(&table->read_set, old_map);
        DBUG_RETURN(HA_ERR_INTERNAL_ERROR);
      }
      se->add_insert_column(field_converters[i]->field->field_name, 0,
                            cass_data, cass_data_len);
    }
  }
  if (dyncol_set)
  {
    /* find removed fields */
    uint i= 0, j= 0;
    /* both array are sorted */
    for(; i < oldcount; i++)
    {
      int scmp= 0;
      while (j < count &&
             (scmp = mariadb_dyncol_column_cmp_named(names + j,
                                                     oldnames + i)) < 0)
        j++;
      if (j < count &&
          scmp == 0)
        j++;
      else
        se->add_insert_delete_column(oldnames[i].str, oldnames[i].length);
    }
  }

  dbug_tmp_restore_column_map(&table->read_set, old_map);

  res= se->do_insert();

  if (res)
    my_error(ER_INTERNAL_ERROR, MYF(0), se->error_str());

err:
  if (dyncol_set)
  {
    free_dynamic_row(&oldvals, &oldnames);
    free_dynamic_row(&vals, &names);
  }

  DBUG_RETURN(res? HA_ERR_INTERNAL_ERROR: 0);
}


/*
  We can't really have any locks for Cassandra Storage Engine. We're reading
  from Cassandra cluster, and other clients can asynchronously modify the data.
  
  We can enforce locking within this process, but this will not be useful. 
 
  Thus, store_lock() should express that:
  - Writes do not block other writes
  - Reads should not block anything either, including INSERTs.
*/
THR_LOCK_DATA **ha_cassandra::store_lock(THD *thd,
                                         THR_LOCK_DATA **to,
                                         enum thr_lock_type lock_type)
{
  DBUG_ENTER("ha_cassandra::store_lock");
  if (lock_type != TL_IGNORE && lock.type == TL_UNLOCK)
  {
    /* Writes allow other writes */
    if ((lock_type >= TL_WRITE_CONCURRENT_INSERT &&
         lock_type <= TL_WRITE))
      lock_type = TL_WRITE_ALLOW_WRITE;

    /* Reads allow everything, including INSERTs */
    if (lock_type == TL_READ_NO_INSERT)
      lock_type = TL_READ;

    lock.type= lock_type;
  }
  *to++= &lock;
  DBUG_RETURN(to);
}


ha_rows ha_cassandra::records_in_range(uint inx, key_range *min_key,
                                       key_range *max_key)
{
  DBUG_ENTER("ha_cassandra::records_in_range");
  DBUG_RETURN(HA_POS_ERROR); /* Range scans are not supported */
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
  DBUG_ENTER("ha_cassandra::check_if_incompatible_data");
  /* Checked, we intend to have this empty for Cassandra SE. */
  DBUG_RETURN(COMPATIBLE_DATA_YES);
}


void Cassandra_se_interface::print_error(const char *format, ...)
{
  va_list ap;
  va_start(ap, format);
  // it's not a problem if output was truncated
  my_vsnprintf(err_buffer, sizeof(err_buffer), format, ap);
  va_end(ap);
}


struct st_mysql_storage_engine cassandra_storage_engine=
{ MYSQL_HANDLERTON_INTERFACE_VERSION };

static SHOW_VAR cassandra_status_variables[]= {
  {"row_inserts",
    (char*) &cassandra_counters.row_inserts,         SHOW_LONG},
  {"row_insert_batches",
    (char*) &cassandra_counters.row_insert_batches,  SHOW_LONG},

  {"multiget_keys_scanned",
    (char*) &cassandra_counters.multiget_keys_scanned, SHOW_LONG},
  {"multiget_reads",
    (char*) &cassandra_counters.multiget_reads,      SHOW_LONG},
  {"multiget_rows_read",
    (char*) &cassandra_counters.multiget_rows_read,  SHOW_LONG},

  {"network_exceptions",
    (char*) &cassandra_counters.network_exceptions, SHOW_LONG},
  {"timeout_exceptions",
    (char*) &cassandra_counters.timeout_exceptions, SHOW_LONG},
  {"unavailable_exceptions",
    (char*) &cassandra_counters.unavailable_exceptions, SHOW_LONG},
  {NullS, NullS, SHOW_LONG}
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
  0x0001,                                        /* version number (0.1) */
  cassandra_status_variables,                     /* status variables */
  cassandra_system_variables,                     /* system variables */
  "0.1",                                        /* string version */
  MariaDB_PLUGIN_MATURITY_EXPERIMENTAL          /* maturity */
}
maria_declare_plugin_end;
