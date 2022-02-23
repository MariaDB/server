/* Copyright (C) 2009 MySQL AB
   Copyright (c) 2019, 2020, MariaDB Corporation.

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

/**
  @file

  @brief
  functions to update persitent statistical tables and to read from them

  @defgroup Query_Optimizer  Query Optimizer
  @{
*/

#include "mariadb.h"
#include "sql_base.h"
#include "key.h"
#include "sql_statistics.h"
#include "opt_histogram_json.h"
#include "opt_range.h"
#include "uniques.h"
#include "sql_show.h"
#include "sql_partition.h"

#include <vector>
#include <string>

/*
  The system variable 'use_stat_tables' can take one of the
  following values:
  "never", "complementary", "preferably". 
  If the values of the variable 'use_stat_tables' is set to
  "never then any statistical data from  the persistent statistical tables
  is ignored by the optimizer.
  If the value of the variable 'use_stat_tables' is set to
  "complementary" then a particular statistical characteristic is used
  by the optimizer only if the database engine does not provide similar
  statistics. For example, 'nulls_ratio' for table columns  currently 
  are not provided by any engine. So optimizer uses this statistical data
  from the  statistical tables. At the same time it does not use 
  'avg_frequency' for any index prefix from the statistical tables since
  the a similar statistical characteristic 'records_per_key' can be
  requested from the database engine.
  If the value the variable 'use_stat_tables' is set to
  "preferably" the optimizer uses a particular statistical data only if
  it can't be found in the statistical data.
  If an ANALYZE command is executed then it results in collecting
  statistical data for the tables specified by the command and storing
  the collected statistics in the persistent statistical tables only
  when the value of the variable 'use_stat_tables' is not
  equal to "never".
*/

Histogram_base *create_histogram(MEM_ROOT *mem_root, Histogram_type hist_type,
                                 THD *owner);

/* Currently there are only 3 persistent statistical tables */
static const uint STATISTICS_TABLES= 3;

/* 
  The names of the statistical tables in this array must correspond the
  definitions of the tables in the file ../scripts/mysql_system_tables.sql
*/
static const LEX_CSTRING stat_table_name[STATISTICS_TABLES]=
{
  { STRING_WITH_LEN("table_stats") },
  { STRING_WITH_LEN("column_stats") },
  { STRING_WITH_LEN("index_stats") }
};


/**
  @details
  The function builds a list of TABLE_LIST elements for system statistical
  tables using array of TABLE_LIST passed as a parameter. 
  The lock type of each element is set to TL_READ if for_write = FALSE,
  otherwise it is set to TL_WRITE.
*/

static
inline void init_table_list_for_stat_tables(TABLE_LIST *tables, bool for_write)
{
  uint i;

  memset((char *) &tables[0], 0, sizeof(TABLE_LIST) * STATISTICS_TABLES);

  for (i= 0; i < STATISTICS_TABLES; i++)
  {
    tables[i].db= MYSQL_SCHEMA_NAME;
    tables[i].table_name= stat_table_name[i];
    tables[i].alias=      stat_table_name[i];
    tables[i].lock_type= for_write ? TL_WRITE : TL_READ;
    if (i < STATISTICS_TABLES - 1)
    tables[i].next_global= tables[i].next_local=
      tables[i].next_name_resolution_table= &tables[i+1];
    if (i != 0)
      tables[i].prev_global= &tables[i-1].next_global;
  }
}

static Table_check_intact_log_error stat_table_intact;

static const
TABLE_FIELD_TYPE table_stat_fields[TABLE_STAT_N_FIELDS] =
{
  {
    { STRING_WITH_LEN("db_name") },
    { STRING_WITH_LEN("varchar(64)") },
    { STRING_WITH_LEN("utf8mb3") }
  },
  {
    { STRING_WITH_LEN("table_name") },
    { STRING_WITH_LEN("varchar(64)") },
    { STRING_WITH_LEN("utf8mb3") }
  },
  {
    { STRING_WITH_LEN("cardinality") },
    { STRING_WITH_LEN("bigint(21)") },
    { NULL, 0 }
  },
};
static const uint table_stat_pk_col[]= {0,1};
static const TABLE_FIELD_DEF
table_stat_def= {TABLE_STAT_N_FIELDS, table_stat_fields, 2, table_stat_pk_col };

static const
TABLE_FIELD_TYPE column_stat_fields[COLUMN_STAT_N_FIELDS] =
{
  {
    { STRING_WITH_LEN("db_name") },
    { STRING_WITH_LEN("varchar(64)") },
    { STRING_WITH_LEN("utf8mb3") }
  },
  {
    { STRING_WITH_LEN("table_name") },
    { STRING_WITH_LEN("varchar(64)") },
    { STRING_WITH_LEN("utf8mb3") }
  },
  {
    { STRING_WITH_LEN("column_name") },
    { STRING_WITH_LEN("varchar(64)") },
    { STRING_WITH_LEN("utf8mb3") }
  },
  {
    { STRING_WITH_LEN("min_value") },
    { STRING_WITH_LEN("varbinary(255)") },
    { NULL, 0 }
  },
  {
    { STRING_WITH_LEN("max_value") },
    { STRING_WITH_LEN("varbinary(255)") },
    { NULL, 0 }
  },
  {
    { STRING_WITH_LEN("nulls_ratio") },
    { STRING_WITH_LEN("decimal(12,4)") },
    { NULL, 0 }
  },
  {
    { STRING_WITH_LEN("avg_length") },
    { STRING_WITH_LEN("decimal(12,4)") },
    { NULL, 0 }
  },
  {
    { STRING_WITH_LEN("avg_frequency") },
    { STRING_WITH_LEN("decimal(12,4)") },
    { NULL, 0 }
  },
  {
    { STRING_WITH_LEN("hist_size") },
    { STRING_WITH_LEN("tinyint(3)") },
    { NULL, 0 }
  },
  {
    { STRING_WITH_LEN("hist_type") },
    { STRING_WITH_LEN("enum('SINGLE_PREC_HB','DOUBLE_PREC_HB','JSON_HB')") },
    { STRING_WITH_LEN("utf8mb3") }
  },
  {
    { STRING_WITH_LEN("histogram") },
    { STRING_WITH_LEN("longblob") },
    { NULL, 0 }
  }
};
static const uint column_stat_pk_col[]= {0,1,2};
static const TABLE_FIELD_DEF
column_stat_def= {COLUMN_STAT_N_FIELDS, column_stat_fields, 3, column_stat_pk_col};

static const
TABLE_FIELD_TYPE index_stat_fields[INDEX_STAT_N_FIELDS] =
{
  {
    { STRING_WITH_LEN("db_name") },
    { STRING_WITH_LEN("varchar(64)") },
    { STRING_WITH_LEN("utf8mb3") }
  },
  {
    { STRING_WITH_LEN("table_name") },
    { STRING_WITH_LEN("varchar(64)") },
    { STRING_WITH_LEN("utf8mb3") }
  },
  {
    { STRING_WITH_LEN("index") },
    { STRING_WITH_LEN("varchar(64)") },
    { STRING_WITH_LEN("utf8mb3") }
  },
  {
    { STRING_WITH_LEN("prefix_arity") },
    { STRING_WITH_LEN("int(11)") },
    { NULL, 0 }
  },
  {
    { STRING_WITH_LEN("avg_frequency") },
    { STRING_WITH_LEN("decimal(12,4)") },
    { NULL, 0 }
  }
};
static const uint index_stat_pk_col[]= {0,1,2,3};
static const TABLE_FIELD_DEF
index_stat_def= {INDEX_STAT_N_FIELDS, index_stat_fields, 4, index_stat_pk_col};


/**
  @brief
  Open all statistical tables and lock them
*/

static int open_stat_tables(THD *thd, TABLE_LIST *tables, bool for_write)
{
  int rc;
  Dummy_error_handler deh; // suppress errors
  DBUG_ASSERT(thd->internal_transaction());

  thd->push_internal_handler(&deh);
  init_table_list_for_stat_tables(tables, for_write);
  init_mdl_requests(tables);
  thd->in_sub_stmt|= SUB_STMT_STAT_TABLES;
  rc= open_system_tables_for_read(thd, tables);
  thd->in_sub_stmt&= ~SUB_STMT_STAT_TABLES;
  thd->pop_internal_handler();


  /* If the number of tables changes, we should revise the check below. */
  compile_time_assert(STATISTICS_TABLES == 3);

  if (!rc &&
      (stat_table_intact.check(tables[TABLE_STAT].table, &table_stat_def) ||
       stat_table_intact.check(tables[COLUMN_STAT].table, &column_stat_def) ||
       stat_table_intact.check(tables[INDEX_STAT].table, &index_stat_def)))
  {
    close_thread_tables(thd);
    rc= 1;
  }

  return rc;
} 


/**
  @brief
  Open a statistical table and lock it

  @details
  This is used by DDLs. When a column or index is dropped or renamed,
  stat tables need to be adjusted accordingly.
*/
static inline int open_stat_table_for_ddl(THD *thd, TABLE_LIST *table,
                                          const LEX_CSTRING *stat_tab_name)
{
  table->init_one_table(&MYSQL_SCHEMA_NAME, stat_tab_name, NULL, TL_WRITE);
  No_such_table_error_handler nst_handler;
  thd->push_internal_handler(&nst_handler);
  int res= open_system_tables_for_read(thd, table);
  thd->pop_internal_handler();
  return res;
}


/*
  The class Column_statistics_collected is a helper class used to collect
  statistics on a table column. The class is derived directly from
  the class Column_statistics, and, additionally to the fields of the
  latter, it contains the fields to accumulate the results of aggregation
  for the number of nulls in the column and for the size of the column
  values. There is also a container for distinct column values used
  to calculate the average number of records per distinct column value. 
*/ 

class Column_statistics_collected :public Column_statistics
{

private:
  Field *column;  /* The column to collect statistics on */
  ha_rows nulls;  /* To accumulate the number of nulls in the column */ 
  ulonglong column_total_length; /* To accumulate the size of column values */
  Count_distinct_field *count_distinct; /* The container for distinct 
                                           column values */

  bool is_single_pk_col; /* TRUE <-> the only column of the primary key */ 

public:

  inline void init(THD *thd, Field * table_field);
  inline bool add();
  inline bool finish(MEM_ROOT *mem_root, ha_rows rows, double sample_fraction);
  inline void cleanup();
};


/**
  Stat_table is the base class for classes Table_stat, Column_stat and
  Index_stat. The methods of these classes allow us to read statistical
  data from statistical tables, write collected statistical data into
  statistical tables and update statistical data in these  tables
  as well as update access fields belonging to the primary key and
  delete records by prefixes of the primary key.
  Objects of the classes Table_stat, Column_stat  and Index stat are used 
  for reading/writing statistics from/into persistent tables table_stats,
  column_stats and index_stats correspondingly.  These tables are stored in
  the system database 'mysql'.

  Statistics is read and written always for a given database table t. When
  an  object of any of these classes is created a pointer to the TABLE
  structure for this database table is passed as a parameter to the constructor
  of the object. The other parameter is a pointer to the TABLE structure for
  the corresponding statistical table st. So construction of an object to 
  read/write statistical data on table t from/into statistical table st 
  requires both table t and st to be opened.
  In some cases the TABLE structure for table t may be undefined. Then
  the objects of the classes Table_stat, Column_stat  and Index stat are
  created by the alternative constructor that require only the name
  of the table t and the name of the database it belongs to. Currently the
  alternative constructors are used only in the cases when some records
  belonging to the table are to be deleted, or its keys are to be updated   

  Reading/writing statistical data from/into a statistical table is always
  performed by a key.  At the moment there is only one key defined for each
  statistical table and this key is primary.
  The primary key for the table table_stats is built as (db_name, table_name).
  The primary key for the table column_stats is built as (db_name, table_name,
  column_name).
  The primary key for the table index_stats is built as (db_name, table_name,
  index_name, prefix_arity).

  Reading statistical data from a statistical table is performed by the 
  following pattern. First a table dependent method sets the values of the
  the fields that comprise the lookup key. Then an implementation of the 
  method get_stat_values() declared in Stat_table as a pure virtual method
  finds the row from the statistical table by the set key. If the row is
  found the values of statistical fields are read from this row and are
  distributed in the internal structures.

  Let's assume the statistical data is read for table t from database db.

  When statistical data is searched in the table table_stats first 
  Table_stat::set_key_fields() should set the fields of db_name and
  table_name. Then get_stat_values looks for a row by the set key value,
  and, if the row is found, reads the value from the column
  table_stats.cardinality into the field read_stat.cardinality of the TABLE
  structure for table t and sets the value of read_stat.cardinality_is_null
  from this structure to FALSE. If the value of the 'cardinality' column
  in the row is null or if no row is found read_stat.cardinality_is_null
  is set to TRUE.

  When statistical data is searched in the table column_stats first
  Column_stat::set_key_fields() should set the fields of db_name, table_name
  and column_name with column_name taken out of the only parameter f of the
  Field* type passed to this method. After this get_stat_values looks
  for a row by the set key value. If the row is found the values of statistical 
  data columns min_value, max_value, nulls_ratio, avg_length, avg_frequency,
  hist_size, hist_type, histogram are read into internal structures. Values
  of nulls_ratio, avg_length, avg_frequency, hist_size, hist_type, histogram
  are read into the corresponding fields of the read_stat  structure from
  the Field object f, while values from min_value and max_value  are copied
  into the min_value and  max_value record buffers attached to the TABLE
   structure for table t.
  If the value of a statistical column in the found row is null, then the
  corresponding flag in the f->read_stat.column_stat_nulls bitmap is set off.
  Otherwise the flag is set on. If no row is found for the column the all flags
  in f->column_stat_nulls are set off.
  
  When statistical data is searched in the table index_stats first
  Index_stat::set_key_fields() has to be called to set the fields of db_name,
  table_name, index_name and prefix_arity. The value of index_name is extracted
  from the first parameter key_info of the KEY* type passed to the method.
  This parameter  specifies the index of interest idx. The second parameter
  passed to the method specifies the arity k of the index prefix for which
  statistical data is to be read. E.g. if the index idx consists of 3
  components (p1,p2,p3) the table  index_stats usually will contain 3 rows for
  this index: the first - for the prefix (p1), the second - for the prefix
  (p1,p2), and the third - for the the prefix (p1,p2,p3). After the key fields
  has been set a call of get_stat_value looks for a row by the set key value.
  If the row is found and the value of the avg_frequency column is not null 
  then this value is assigned to key_info->read_stat.avg_frequency[k].
  Otherwise 0 is assigned to this element. 

  The method Stat_table::update_stat is used to write statistical data
  collected in the internal structures into a statistical table st.
  It is assumed that before any invocation of this method a call of the
  function st.set_key_fields has set the values of the primary key fields
  that serve to locate the row from the statistical table st where the 
  the collected statistical data from internal structures are to be written
  to. The statistical data is written from the counterparts of the
  statistical fields of internal structures into which it would be read
  by the functions get_stat_values. The counterpart fields are used
  only when statistics is collected
  When updating/inserting a row from the statistical table st the method
  Stat_table::update_stat calls the implementation of the pure virtual
  method store_field_values to transfer statistical data from the fields
  of internal structures to the fields of record buffer used for updates
  of the statistical table st.     
*/  
         
class Stat_table 
{

private:

  /* Handler used for the retrieval of the statistical table stat_table */
  handler *stat_file;
  
  uint stat_key_length; /* Length of the key to access stat_table */
  uchar *record[2];     /* Record buffers used to access/update stat_table */
  uint stat_key_idx;    /* The number of the key to access stat_table */

  /* This is a helper function used only by the Stat_table constructors */
  void common_init_stat_table()
  {
    stat_file= stat_table->file;
    /* Currently any statistical table has only one key */
    stat_key_idx= 0;
    stat_key_info= &stat_table->key_info[stat_key_idx];
    stat_key_length= stat_key_info->key_length;
    record[0]= stat_table->record[0];
    record[1]= stat_table->record[1];
  }

protected:

  /* Statistical table to read statistics from or to update/delete */
  TABLE *stat_table;
  KEY *stat_key_info;   /* Structure for the index to access stat_table */
  
  /* Table for which statistical data is read / updated */
  TABLE *table;
  TABLE_SHARE *table_share; /* Table share for 'table */
  const LEX_CSTRING *db_name;      /* Name of the database containing 'table' */
  const LEX_CSTRING *table_name;   /* Name of the table 'table' */

  void store_record_for_update()
  {
    store_record(stat_table, record[1]);
  }

  void store_record_for_lookup()
  {
    DBUG_ASSERT(record[0] == stat_table->record[0]);
  }

  bool update_record()
  {
    int err;
    if ((err= stat_file->ha_update_row(record[1], record[0])) &&
         err != HA_ERR_RECORD_IS_THE_SAME)
      return TRUE;
    /* Make change permanent and avoid 'table is marked as crashed' errors */
    stat_file->extra(HA_EXTRA_FLUSH);
    return FALSE;
  }

public:


  /**
    @details
    This constructor has to be called by any constructor of the derived
    classes. The constructor 'tunes' the private and protected members of
    the constructed object to the statistical table 'stat_table' with the
    statistical data of our interest and to the table 'tab' for which this
    statistics has been collected.
  */  

  Stat_table(TABLE *stat, TABLE *tab) 
    :stat_table(stat), table(tab)
  {
    table_share= tab->s;
    common_init_stat_table();
    db_name= &table_share->db;
    table_name= &table_share->table_name;
  }


  /**
    @details
    This constructor has to be called by any constructor of the derived
    classes. The constructor 'tunes' the private and protected members of
    the constructed object to the statistical table 'stat_table' with the
    statistical data of our interest and to the table t for which this
    statistics has been collected. The table t is uniquely specified
    by the database name 'db' and the table name 'tab'.
  */  
  
  Stat_table(TABLE *stat, const LEX_CSTRING *db, const LEX_CSTRING *tab)
    :stat_table(stat), table_share(NULL),db_name(db), table_name(tab)
  {
    common_init_stat_table();
  } 


  virtual ~Stat_table() {}

  /**
    @brief
    Store the given values of fields for database name and table name 
   
    @details
    This is a purely virtual method.
    The implementation for any derived class shall store the given
    values of the database name and table name in the corresponding
    fields of stat_table.
    
    @note
    The method is called by the update_table_name_key_parts function.
  */      

 virtual void change_full_table_name(const LEX_CSTRING *db, const LEX_CSTRING *tab)= 0;

 
  /**
    @brief
    Store statistical data into fields of the statistical table
   
    @details
    This is a purely virtual method.
    The implementation for any derived class shall put the appropriate
    statistical data into the corresponding fields of stat_table.
    
    @note
    The method is called by the update_stat function.
  */      

  virtual void store_stat_fields()= 0;

  
  /**
    @brief
    Read statistical data from fields of the statistical table
   
    @details
    This is a purely virtual method.
    The implementation for any derived read shall read the appropriate
    statistical data from the corresponding fields of stat_table.    
  */      
  
  virtual void get_stat_values()= 0;


  /**
    @brief
    Find a record in the statistical table by a primary key

    @details
    The function looks for a record in stat_table by its primary key.
    It assumes that the key fields have been already stored in the record
    buffer of stat_table.

    @retval
    FALSE    the record is not found
    @retval
    TRUE     the record is found
  */

  bool find_stat()
  {
    uchar key[MAX_KEY_LENGTH];
    key_copy(key, record[0], stat_key_info, stat_key_length);
    return !stat_file->ha_index_read_idx_map(record[0], stat_key_idx, key,
                                             HA_WHOLE_KEY, HA_READ_KEY_EXACT);
  }

 
  /**
    @brief
    Find a record in the statistical table by a key prefix value 

    @details
    The function looks for a record in stat_table by the key value consisting
    of 'prefix_parts' major components for the primary index.  
    It assumes that the key prefix fields have been already stored in the record
    buffer of stat_table.

    @retval
    FALSE    the record is not found
    @retval
    TRUE     the record is found
  */

  bool find_next_stat_for_prefix(uint prefix_parts)
  {
    uchar key[MAX_KEY_LENGTH];
    uint prefix_key_length= 0;
    for (uint i= 0; i < prefix_parts; i++)
      prefix_key_length+= stat_key_info->key_part[i].store_length;
    key_copy(key, record[0], stat_key_info, prefix_key_length);
    key_part_map prefix_map= (key_part_map) ((1 << prefix_parts) - 1);
    return !stat_file->ha_index_read_idx_map(record[0], stat_key_idx, key,
                                             prefix_map, HA_READ_KEY_EXACT);
  }
   

  /**
    @brief
    Update/insert a record in the statistical table with new statistics

    @details
    The function first looks for a record by its primary key in the statistical
    table stat_table. If the record is found the function updates statistical
    fields of the records. The data for these fields are taken from internal
    structures containing info on the table 'table'. If the record is not
    found the function inserts a new record with the primary key set to the
    search key and the statistical data taken from the internal structures.
    The function assumes that the key fields have been already stored in
    the record buffer of stat_table.

    @retval
    FALSE    success with the update/insert of the record
    @retval
    TRUE     failure with the update/insert of the record

    @note
    The function calls the virtual method store_stat_fields to populate the
    statistical fields of the updated/inserted row with new statistics.
  */

  bool update_stat()
  {
    if (find_stat())
    {    
      bool res;
      store_record_for_update();
      store_stat_fields();
      res= update_record();
      DBUG_ASSERT(res == 0);
      return res;
    }
    else
    {
      int err;
      store_stat_fields();
      if ((err= stat_file->ha_write_row(record[0])))
      {
        DBUG_ASSERT(0);
	return TRUE;
      }
      /* Make change permanent and avoid 'table is marked as crashed' errors */
      stat_file->extra(HA_EXTRA_FLUSH);
    } 
    return FALSE;
  }


  /** 
    @brief
    Update the table name fields in the current record of stat_table

    @details
    The function updates the fields containing database name and table name
    for the last found record in the statistical table stat_table.
    The corresponding names for update is taken from the parameters
    db and tab.
 
    @retval
    FALSE    success with the update of the record
    @retval
    TRUE     failure with the update of the record

    @note
    The function calls the virtual method change_full_table_name  
    to store the new names in the record buffer used for updates.
  */

  bool update_table_name_key_parts(const LEX_CSTRING *db, const LEX_CSTRING *tab)
  {
    store_record_for_update();
    change_full_table_name(db, tab);
    bool rc= update_record();
    store_record_for_lookup();
    return rc;
  }   


  /** 
    @brief
    Delete the current record of the statistical table stat_table

    @details
    The function deletes the last found record from the statistical
    table stat_table.
 
    @retval
    FALSE    success with the deletion of the record
    @retval
    TRUE     failure with the deletion of the record
  */

  bool delete_stat()
  {
    int err;
    if ((err= stat_file->ha_delete_row(record[0])))
      return TRUE;
    /* Make change permanent and avoid 'table is marked as crashed' errors */
    stat_file->extra(HA_EXTRA_FLUSH);
    return FALSE;
  } 

  friend class Stat_table_write_iter;
};


/*
  An object of the class Table_stat is created to read statistical
  data on tables from the statistical table table_stats, to update
  table_stats with such statistical data, or to update columns
  of the primary key, or to delete the record by its primary key or
  its prefix. 
  Rows from the statistical table are read and updated always by
  primary key. 
*/

class Table_stat: public Stat_table
{

private:

  Field *db_name_field;     /* Field for the column table_stats.db_name */
  Field *table_name_field;  /* Field for the column table_stats.table_name */

  void common_init_table_stat()
  {  
    db_name_field= stat_table->field[TABLE_STAT_DB_NAME];
    table_name_field= stat_table->field[TABLE_STAT_TABLE_NAME];
  }

  void change_full_table_name(const LEX_CSTRING *db, const LEX_CSTRING *tab)
  {
    db_name_field->store(db->str, db->length, system_charset_info);
    table_name_field->store(tab->str, tab->length, system_charset_info);
  }

public:

  /**
    @details
    The constructor 'tunes' the private and protected members of the
    constructed object for the statistical table table_stats to read/update
    statistics on table 'tab'. The TABLE structure for the table table_stat
    must be passed as a value for the parameter 'stat'.
  */

  Table_stat(TABLE *stat, TABLE *tab) :Stat_table(stat, tab)
  {
    common_init_table_stat();
  }


  /**
    @details
    The constructor 'tunes' the private and protected members of the
    object constructed for the statistical table table_stat for 
    the future updates/deletes of the record concerning the table 'tab'
    from the database 'db'.
  */

  Table_stat(TABLE *stat, const LEX_CSTRING *db, const LEX_CSTRING *tab)
    :Stat_table(stat, db, tab)
  {
    common_init_table_stat();
  }


  /** 
    @brief
    Set the key fields for the statistical table table_stat

    @details
    The function sets the values of the fields db_name and table_name
    in the record buffer for the statistical table table_stat.
    These fields comprise the primary key for the table.

    @note
    The function is supposed to be called before any use of the  
    method find_stat for an object of the Table_stat class. 
  */

  void set_key_fields()
  {
    db_name_field->store(db_name->str, db_name->length, system_charset_info);
    table_name_field->store(table_name->str, table_name->length,
                            system_charset_info);
  }


  /** 
    @brief
    Store statistical data into statistical fields of table_stat

    @details
    This implementation of a purely virtual method sets the value of the
    column 'cardinality' of the statistical table table_stat according to
    the value of the flag write_stat.cardinality_is_null and the value of
    the field write_stat.cardinality' from the TABLE structure for 'table'.
  */    

  void store_stat_fields()
  {
    Field *stat_field= stat_table->field[TABLE_STAT_CARDINALITY];
    if (table->collected_stats->cardinality_is_null)
      stat_field->set_null();
    else
    {
      stat_field->set_notnull();
      stat_field->store(table->collected_stats->cardinality,true);
    }
  }


  /**
    @brief
    Read statistical data from statistical fields of table_stat

    @details
    This implementation of a purely virtual method first looks for a record
    the statistical table table_stat by its primary key set the record
    buffer with the help of Table_stat::set_key_fields.  Then, if the row is
    found the function reads the value of the column 'cardinality' of the table
    table_stat and sets the value of the flag read_stat.cardinality_is_null
    and the value of the field read_stat.cardinality' from the TABLE structure
    for 'table' accordingly.
  */    

  void get_stat_values()
  {
    Table_statistics *read_stats= table_share->stats_cb.table_stats;
    read_stats->cardinality_is_null= TRUE;
    read_stats->cardinality= 0;
    if (find_stat())
    {
      Field *stat_field= stat_table->field[TABLE_STAT_CARDINALITY];
      if (!stat_field->is_null())
      {
        read_stats->cardinality_is_null= FALSE;
        read_stats->cardinality= stat_field->val_int();
      }
    }
  } 

};


/*
  An object of the class Column_stat is created to read statistical data
  on table columns from the statistical table column_stats, to update
  column_stats with such statistical data, or to update columns
  of the primary key, or to delete the record by its primary key or
  its prefix.
  Rows from the statistical table are read and updated always by 
  primary key.
*/ 

class Column_stat: public Stat_table
{

private:

  Field *db_name_field;     /* Field for the column column_stats.db_name */
  Field *table_name_field;  /* Field for the column column_stats.table_name */
  Field *column_name_field; /* Field for the column column_stats.column_name */

  Field *table_field;  /* Field from 'table' to read /update statistics on */

  void common_init_column_stat_table()
  {
    db_name_field= stat_table->field[COLUMN_STAT_DB_NAME];
    table_name_field= stat_table->field[COLUMN_STAT_TABLE_NAME];
    column_name_field= stat_table->field[COLUMN_STAT_COLUMN_NAME];
  } 

  void change_full_table_name(const LEX_CSTRING *db, const LEX_CSTRING *tab)
  {
     db_name_field->store(db->str, db->length, system_charset_info);
     table_name_field->store(tab->str, tab->length, system_charset_info);
  }

public:

  /**
    @details
    The constructor 'tunes' the private and protected members of the
    constructed object for the statistical table column_stats to read/update
    statistics on fields of the table 'tab'. The TABLE structure for the table
    column_stats must be passed as a value for the parameter 'stat'.
  */

  Column_stat(TABLE *stat, TABLE *tab) :Stat_table(stat, tab)
  {
    common_init_column_stat_table();
  } 


  /**
    @details
    The constructor 'tunes' the private and protected members of the
    object constructed for the statistical table column_stats for 
    the future updates/deletes of the record concerning the table 'tab'
    from the database 'db'. 
  */

  Column_stat(TABLE *stat, const LEX_CSTRING *db, const LEX_CSTRING *tab)
    :Stat_table(stat, db, tab)
  {
    common_init_column_stat_table();
  } 

  /** 
    @brief
    Set table name fields for the statistical table column_stats

    @details
    The function stores the values of the fields db_name and table_name 
    of the statistical table column_stats in the record buffer.
  */

  void set_full_table_name()
  {
    db_name_field->store(db_name->str, db_name->length, system_charset_info);
    table_name_field->store(table_name->str, table_name->length,
                            system_charset_info);
  }


  /** 
    @brief
    Set the key fields for the statistical table column_stats

    @param
    col       Field for the 'table' column to read/update statistics on

    @details
    The function stores the values of the fields db_name, table_name and
    column_name in the record buffer for the statistical table column_stats.
    These fields comprise the primary key for the table.
    It also sets table_field to the passed parameter.

    @note
    The function is supposed to be called before any use of the  
    method find_stat for an object of the Column_stat class.
  */

  void set_key_fields(Field *col)
  {
    set_full_table_name();
    column_name_field->store(col->field_name.str, col->field_name.length,
                             system_charset_info);  
    table_field= col;
  }


  /** 
    @brief
    Update the table name fields in the current record of stat_table

    @details
    The function updates the primary key fields containing database name,
    table name, and column name for the last found record in the statistical
    table column_stats.
    
    @retval
    FALSE    success with the update of the record
    @retval
    TRUE     failure with the update of the record
  */

  bool update_column_key_part(const char *col)
  {
    store_record_for_update();
    set_full_table_name();
    column_name_field->store(col, strlen(col), system_charset_info);
    bool rc= update_record();
    store_record_for_lookup();
    return rc;
  }   


  /** 
    @brief
    Store statistical data into statistical fields of column_stats

    @details
    This implementation of a purely virtual method sets the value of the
    columns 'min_value', 'max_value', 'nulls_ratio', 'avg_length',
    'avg_frequency', 'hist_size', 'hist_type' and 'histogram'  of the 
    stistical table columns_stat according to the contents of the bitmap
    write_stat.column_stat_nulls and the values of the fields min_value,
    max_value, nulls_ratio, avg_length, avg_frequency, hist_size, hist_type
    and histogram of the structure write_stat from the Field structure
    for the field 'table_field'.
    The value of the k-th column in the table columns_stat is set to NULL
    if the k-th bit in the bitmap 'column_stat_nulls' is set to 1. 

    @note
    A value from the field min_value/max_value is always converted
    into a varbinary string. If the length of the column 'min_value'/'max_value'
    is less than the length of the string the string is trimmed to fit the
    length of the column. 
  */    

  void store_stat_fields()
  {
    StringBuffer<MAX_FIELD_WIDTH> val;

    MY_BITMAP *old_map= dbug_tmp_use_all_columns(stat_table, &stat_table->read_set);
    for (uint i= COLUMN_STAT_MIN_VALUE; i <= COLUMN_STAT_HISTOGRAM; i++)
    {  
      Field *stat_field= stat_table->field[i];
      Column_statistics *stats= table_field->collected_stats;
      if (stats->is_null(i))
        stat_field->set_null();
      else
      {
        stat_field->set_notnull();
        switch (i) {
        case COLUMN_STAT_MIN_VALUE:
        {
          /*
            TODO varun: After MDEV-22583 is fixed, add a function in Field_bit
            and move this implementation there
          */
          if (table_field->type() == MYSQL_TYPE_BIT)
            stat_field->store(stats->min_value->val_int(),true);
          else
            stats->min_value->store_to_statistical_minmax_field(stat_field, &val);
          break;
        }
        case COLUMN_STAT_MAX_VALUE:
        {
          if (table_field->type() == MYSQL_TYPE_BIT)
            stat_field->store(stats->max_value->val_int(),true);
          else
            stats->max_value->store_to_statistical_minmax_field(stat_field, &val);
          break;
        }
        case COLUMN_STAT_NULLS_RATIO:
          stat_field->store(stats->get_nulls_ratio());
          break;
        case COLUMN_STAT_AVG_LENGTH:
          stat_field->store(stats->get_avg_length());
          break;
        case COLUMN_STAT_AVG_FREQUENCY:
          stat_field->store(stats->get_avg_frequency());
          break; 
        case COLUMN_STAT_HIST_SIZE:
          // Note: this is dumb. the histogram size is stored with the
          // histogram!
          stat_field->store(stats->histogram?
                              stats->histogram->get_size() : 0);
          break;
        case COLUMN_STAT_HIST_TYPE:
          if (stats->histogram)
            stat_field->store(stats->histogram->get_type() + 1);
          else
            stat_field->set_null();
          break;
        case COLUMN_STAT_HISTOGRAM:
          if (stats->histogram)
            stats->histogram->serialize(stat_field);
          else
            stat_field->set_null();
          break;
        }
      }
    }
    dbug_tmp_restore_column_map(&stat_table->read_set, old_map);
  }


  /** 
    @brief
    Read statistical data from statistical fields of column_stats

    @details
    This implementation of a purely virtual method first looks for a record
    in the statistical table column_stats by its primary key set in the record
    buffer with the help of Column_stat::set_key_fields. Then, if the row is
    found, the function reads the values of the columns 'min_value',
    'max_value', 'nulls_ratio', 'avg_length', 'avg_frequency', 'hist_size' and
    'hist_type" of the  table column_stat and sets accordingly the value of
    the bitmap  read_stat.column_stat_nulls' and the values of the fields
    min_value, max_value, nulls_ratio, avg_length, avg_frequency, hist_size and
    hist_type of the structure read_stat from the Field structure for the field
    'table_field'.
  */    

  void get_stat_values()
  {
    table_field->read_stats->set_all_nulls();
    // default: hist_type=NULL means there's no histogram
    table_field->read_stats->histogram_type_on_disk= INVALID_HISTOGRAM;

    if (table_field->read_stats->min_value)
      table_field->read_stats->min_value->set_null();
    if (table_field->read_stats->max_value)
      table_field->read_stats->max_value->set_null();

    if (find_stat())
    {
      char buff[MAX_FIELD_WIDTH];
      String val(buff, sizeof(buff), &my_charset_bin);

      for (uint i= COLUMN_STAT_MIN_VALUE; i <= COLUMN_STAT_HISTOGRAM; i++)
      {  
        Field *stat_field= stat_table->field[i];

        if (!stat_field->is_null() &&
            (i > COLUMN_STAT_MAX_VALUE ||
             (i == COLUMN_STAT_MIN_VALUE && 
              table_field->read_stats->min_value) ||
             (i == COLUMN_STAT_MAX_VALUE && 
              table_field->read_stats->max_value)))
        {
          table_field->read_stats->set_not_null(i);

          switch (i) {
          case COLUMN_STAT_MIN_VALUE:
          {
            Field *field= table_field->read_stats->min_value;
            field->set_notnull();
            if (table_field->type() == MYSQL_TYPE_BIT)
              field->store(stat_field->val_int(), true);
            else
              field->store_from_statistical_minmax_field(stat_field, &val);
            break;
          }
          case COLUMN_STAT_MAX_VALUE:
          {
            Field *field= table_field->read_stats->max_value;
            field->set_notnull();
            if (table_field->type() == MYSQL_TYPE_BIT)
              field->store(stat_field->val_int(), true);
            else
              field->store_from_statistical_minmax_field(stat_field, &val);
            break;
          }
          case COLUMN_STAT_NULLS_RATIO:
            table_field->read_stats->set_nulls_ratio(stat_field->val_real());
            break;
          case COLUMN_STAT_AVG_LENGTH:
            table_field->read_stats->set_avg_length(stat_field->val_real());
            break;
          case COLUMN_STAT_AVG_FREQUENCY:
            table_field->read_stats->set_avg_frequency(stat_field->val_real());
            break;
          case COLUMN_STAT_HIST_SIZE:
            /*
              Ignore the contents of mysql.column_stats.hist_size. We take the
              size from the mysql.column_stats.histogram column, itself.
            */
            break;            
          case COLUMN_STAT_HIST_TYPE:
            {
              /*
                Save the histogram type. The histogram itself will be read in
                read_histograms_for_table().
              */
              Histogram_type hist_type= (Histogram_type) (stat_field->val_int() -
                                                          1);
              table_field->read_stats->histogram_type_on_disk= hist_type;
              break;
            }
          case COLUMN_STAT_HISTOGRAM:
            /*
              Do nothing here: we take the histogram length from the 'histogram'
              column itself
            */
            break;
          }
        }
      }
    }
  }


  /** 
    @brief
    Read histogram from of column_stats

    @details
    This method first looks for a record in the statistical table column_stats
    by its primary key set the record buffer with the help of
    Column_stat::set_key_fields. Then, if the row is found, the function reads
    the value of the column 'histogram' of the  table column_stat and sets
    accordingly the corresponding bit in the bitmap read_stat.column_stat_nulls.
    The method assumes that the value of histogram size and the pointer to
    the histogram location has been already set in the fields size and values
    of read_stats->histogram.
  */

  Histogram_base * load_histogram(MEM_ROOT *mem_root)
  {
    if (find_stat())
    {
      char buff[MAX_FIELD_WIDTH];
      String val(buff, sizeof(buff), &my_charset_bin);
      uint fldno= COLUMN_STAT_HISTOGRAM;
      Field *stat_field= stat_table->field[fldno];
      table_field->read_stats->set_not_null(fldno);
      stat_field->val_str(&val);
      Histogram_type hist_type=
        table_field->read_stats->histogram_type_on_disk;

      Histogram_base *hist;
      if (!(hist= create_histogram(mem_root, hist_type, NULL)))
        return NULL;
      Field *field= table->field[table_field->field_index];
      if (!hist->parse(mem_root, db_name->str, table_name->str,
                       field, hist_type,
                       val.ptr(), val.length()))
      {
        table_field->read_stats->histogram= hist;
        return hist;
      }
      else
        delete hist;
    }
    return NULL;
  }
};


bool Histogram_binary::parse(MEM_ROOT *mem_root, const char*, const char*,
                             Field*, Histogram_type type_arg,
                             const char *hist_data, size_t hist_data_len)
{
  /* On-disk an in-memory formats are the same. Just copy the data. */
  type= type_arg;
  size= (uint8) hist_data_len; // 'size' holds the size of histogram in bytes
  if (!(values= (uchar*)alloc_root(mem_root, hist_data_len)))
    return true;

  memcpy(values, hist_data, hist_data_len);
  return false;
}

/*
  Save the histogram data info a table field.
*/
void Histogram_binary::serialize(Field *field)
{
  field->store((char*)values, size, &my_charset_bin);
}

void Histogram_binary::init_for_collection(MEM_ROOT *mem_root,
                                           Histogram_type htype_arg,
                                           ulonglong size_arg)
{
  type= htype_arg;
  values= (uchar*)alloc_root(mem_root, (size_t)size_arg);
  size= (uint8) size_arg;
}


/*
  An object of the class Index_stat is created to read statistical
  data on tables from the statistical table table_stat, to update
  index_stats with such statistical data, or to update columns
  of the primary key, or to delete the record by its primary key or
  its prefix. 
  Rows from the statistical table are read and updated always by
  primary key. 
*/ 

class Index_stat: public Stat_table
{

private:

  Field *db_name_field;      /* Field for the column index_stats.db_name */
  Field *table_name_field;   /* Field for the column index_stats.table_name */
  Field *index_name_field;   /* Field for the column index_stats.table_name */
  Field *prefix_arity_field; /* Field for the column index_stats.prefix_arity */

  KEY *table_key_info;  /* Info on the index to read/update statistics on */
  uint prefix_arity; /* Number of components of the index prefix of interest */

  void common_init_index_stat_table()
  {
    db_name_field= stat_table->field[INDEX_STAT_DB_NAME];
    table_name_field= stat_table->field[INDEX_STAT_TABLE_NAME];
    index_name_field= stat_table->field[INDEX_STAT_INDEX_NAME];
    prefix_arity_field= stat_table->field[INDEX_STAT_PREFIX_ARITY];
  } 

  void change_full_table_name(const LEX_CSTRING *db, const LEX_CSTRING *tab)
  {
     db_name_field->store(db->str, db->length, system_charset_info);
     table_name_field->store(tab->str, tab->length, system_charset_info);
  }

public:


  /**
    @details
    The constructor 'tunes' the private and protected members of the
    constructed object for the statistical table index_stats to read/update
    statistics on prefixes of different indexes of the table 'tab'.
    The TABLE structure for the table index_stats must be passed as a value
    for the parameter 'stat'.
  */

  Index_stat(TABLE *stat, TABLE*tab) :Stat_table(stat, tab)
  {
    common_init_index_stat_table();
  }


  /**
    @details
    The constructor 'tunes' the private and protected members of the
    object constructed for the statistical table index_stats for 
    the future updates/deletes of the record concerning the table 'tab'
    from the database 'db'. 
  */

  Index_stat(TABLE *stat, const LEX_CSTRING *db, const LEX_CSTRING *tab)
    :Stat_table(stat, db, tab)
  {
    common_init_index_stat_table();
  }


  /**
    @brief
    Set table name fields for the statistical table index_stats

    @details
    The function stores the values of the fields db_name and table_name 
    of the statistical table index_stats in the record buffer.
  */

  void set_full_table_name()
  {
    db_name_field->store(db_name->str, db_name->length, system_charset_info);
    table_name_field->store(table_name->str, table_name->length,
                            system_charset_info);
  }

  /** 
    @brief
    Set the key fields of index_stats used to access records for index prefixes

    @param
    index_info   Info for the index of 'table' to read/update statistics on

    @details
    The function sets the values of the fields db_name, table_name and
    index_name in the record buffer for the statistical table index_stats. 
    It also sets table_key_info to the passed parameter.

    @note
    The function is supposed to be called before any use of the method
    find_next_stat_for_prefix for an object of the Index_stat class.
  */

  void set_index_prefix_key_fields(KEY *index_info)
  {
    set_full_table_name();
    const char *index_name= index_info->name.str;
    index_name_field->store(index_name, index_info->name.length,
                            system_charset_info);
    table_key_info= index_info;
  }


  /** 
    @brief
    Set the key fields for the statistical table index_stats

    @param
    index_info   Info for the index of 'table' to read/update statistics on
    @param
    index_prefix_arity Number of components in the index prefix of interest

    @details
    The function sets the values of the fields db_name, table_name and
    index_name, prefix_arity in the record buffer for the statistical
    table index_stats. These fields comprise the primary key for the table. 

    @note
    The function is supposed to be called before any use of the  
    method find_stat for an object of the Index_stat class.
  */

  void set_key_fields(KEY *index_info, uint index_prefix_arity)
  {
    set_index_prefix_key_fields(index_info);
    prefix_arity= index_prefix_arity; 
    prefix_arity_field->store(index_prefix_arity, TRUE);  
  }


  /** 
    @brief
    Store statistical data into statistical fields of table index_stats

    @details
    This implementation of a purely virtual method sets the value of the
    column 'avg_frequency' of the statistical table index_stats according to
    the value of write_stat.avg_frequency[Index_stat::prefix_arity]
    from the KEY_INFO structure 'table_key_info'.
    If the value of write_stat. avg_frequency[Index_stat::prefix_arity] is
    equal  to 0, the value of the column is set to NULL.
  */    

  void store_stat_fields()
  {
    Field *stat_field= stat_table->field[INDEX_STAT_AVG_FREQUENCY];
    double avg_frequency=
      table_key_info->collected_stats->get_avg_frequency(prefix_arity-1);
    if (avg_frequency == 0)
      stat_field->set_null();
    else
    {
      stat_field->set_notnull();
      stat_field->store(avg_frequency);
    }
  }


  /** 
    @brief
    Read statistical data from statistical fields of index_stats

    @details
    This implementation of a purely virtual method first looks for a record the
    statistical table index_stats by its primary key set the record buffer with
    the help of Index_stat::set_key_fields. If the row is found the function
    reads the value of the column 'avg_freguency' of the table index_stat and
    sets the value of read_stat.avg_frequency[Index_stat::prefix_arity]
    from the KEY_INFO structure 'table_key_info' accordingly. If the value of
    the column is NULL, read_stat.avg_frequency[Index_stat::prefix_arity] is
    set to 0. Otherwise, read_stat.avg_frequency[Index_stat::prefix_arity] is
    set to the value of the column.
  */    

  void get_stat_values()
  {
    double avg_frequency= 0;
    if(find_stat())
    {
      Field *stat_field= stat_table->field[INDEX_STAT_AVG_FREQUENCY];
      if (!stat_field->is_null())
        avg_frequency= stat_field->val_real();
    }
    table_key_info->read_stats->set_avg_frequency(prefix_arity-1, avg_frequency);
  }  

};


/*
  An iterator to enumerate statistics table rows which allows to modify
  the rows while reading them.

  Used by RENAME TABLE handling to assign new dbname.tablename to statistic
  rows.
*/
class Stat_table_write_iter
{
  Stat_table *owner;
  IO_CACHE io_cache;
  uchar *rowid_buf;
  uint rowid_size;

public:
  Stat_table_write_iter(Stat_table *stat_table_arg)
   : owner(stat_table_arg), rowid_buf(NULL),
     rowid_size(owner->stat_file->ref_length)
  {
     my_b_clear(&io_cache);
  }

  /*
    Initialize the iterator. It will return rows with n_keyparts matching the
    curernt values.

    @return  false - OK
             true  - Error
  */
  bool init(uint n_keyparts)
  {
    if (!(rowid_buf= (uchar*)my_malloc(PSI_INSTRUMENT_ME, rowid_size, MYF(0))))
      return true;

    if (open_cached_file(&io_cache, mysql_tmpdir, TEMP_PREFIX,
                         1024, MYF(MY_WME)))
      return true;

    handler *h= owner->stat_file;
    uchar key[MAX_KEY_LENGTH];
    uint prefix_len= 0;
    for (uint i= 0; i < n_keyparts; i++)
      prefix_len += owner->stat_key_info->key_part[i].store_length;

    key_copy(key, owner->record[0], owner->stat_key_info,
             prefix_len);
    key_part_map prefix_map= (key_part_map) ((1 << n_keyparts) - 1);
    h->ha_index_init(owner->stat_key_idx, false);
    int res= h->ha_index_read_map(owner->record[0], key, prefix_map,
                                  HA_READ_KEY_EXACT);
    if (res)
    {
      reinit_io_cache(&io_cache, READ_CACHE, 0L, 0, 0);
      /* "Key not found" is not considered an error */
      return (res == HA_ERR_KEY_NOT_FOUND)? false: true;
    }

    do {
      h->position(owner->record[0]);
      my_b_write(&io_cache, h->ref, rowid_size);

    } while (!h->ha_index_next_same(owner->record[0], key, prefix_len));

    /* Prepare for reading */
    reinit_io_cache(&io_cache, READ_CACHE, 0L, 0, 0);
    h->ha_index_or_rnd_end();
    if (h->ha_rnd_init(false))
      return true;

    return false;
  }

  /*
     Read the next row.

     @return
        false   OK
        true    No more rows or error.
  */
  bool get_next_row()
  {
    if (!my_b_inited(&io_cache) || my_b_read(&io_cache, rowid_buf, rowid_size))
      return true; /* No more data */

    handler *h= owner->stat_file;
    /*
      We should normally be able to find the row that we have rowid for. If we
      don't, let's consider this an error.
    */
    int res= h->ha_rnd_pos(owner->record[0], rowid_buf);

    return (res==0)? false : true;
  }

  void cleanup()
  {
    if (rowid_buf)
      my_free(rowid_buf);
    rowid_buf= NULL;
    owner->stat_file->ha_index_or_rnd_end();
    close_cached_file(&io_cache);
    my_b_clear(&io_cache);
  }

  ~Stat_table_write_iter()
  {
    /* Ensure that cleanup has been run */
    DBUG_ASSERT(rowid_buf == 0);
  }
};

class Histogram_binary_builder : public Histogram_builder
{
  Field *min_value;        /* pointer to the minimal value for the field   */
  Field *max_value;        /* pointer to the maximal value for the field   */
  Histogram_binary *histogram;  /* the histogram location                  */
  uint hist_width;         /* the number of points in the histogram        */
  double bucket_capacity;  /* number of rows in a bucket of the histogram  */ 
  uint curr_bucket;        /* number of the current bucket to be built     */

public:
  Histogram_binary_builder(Field *col, uint col_len, ha_rows rows)
    : Histogram_builder(col, col_len, rows)
  {
    Column_statistics *col_stats= col->collected_stats;
    min_value= col_stats->min_value;
    max_value= col_stats->max_value;
    histogram= (Histogram_binary*)col_stats->histogram;
    hist_width= histogram->get_width();
    bucket_capacity= (double) records / (hist_width + 1);
    curr_bucket= 0;
  }

  int next(void *elem, element_count elem_cnt) override
  {
    counters.next(elem, elem_cnt);
    ulonglong count= counters.get_count();
    if (curr_bucket == hist_width)
      return 0;
    if (count > bucket_capacity * (curr_bucket + 1))
    {
      column->store_field_value((uchar *) elem, col_length);
      histogram->set_value(curr_bucket,
                           column->pos_in_interval(min_value, max_value));
      curr_bucket++;
      while (curr_bucket != hist_width &&
             count > bucket_capacity * (curr_bucket + 1))
      {
        histogram->set_prev_value(curr_bucket);
        curr_bucket++;
      }
    }
    return 0;
  }
  void finalize() override {}
};


Histogram_builder *Histogram_binary::create_builder(Field *col, uint col_len,
                                                    ha_rows rows)
{
  return new Histogram_binary_builder(col, col_len, rows);
}


Histogram_base *create_histogram(MEM_ROOT *mem_root, Histogram_type hist_type,
                                 THD *owner)
{
  Histogram_base *res= NULL;
  switch (hist_type) {
  case SINGLE_PREC_HB:
  case DOUBLE_PREC_HB:
    res= new Histogram_binary();
    break;
  case JSON_HB:
    res= new Histogram_json_hb();
    break;
  default:
    DBUG_ASSERT(0);
  }

  if (res)
    res->set_owner(owner);
  return res;
}


C_MODE_START

static int histogram_build_walk(void *elem, element_count elem_cnt, void *arg)
{
  Histogram_builder *hist_builder= (Histogram_builder *) arg;
  return hist_builder->next(elem, elem_cnt);
}

int basic_stats_collector_walk(void *elem, element_count count,
                               void *arg)
{
  ((Basic_stats_collector*)arg)->next(elem, count);
  return 0;
}

C_MODE_END
/*
  The class Count_distinct_field is a helper class used to calculate
  the number of distinct values for a column. The class employs the
  Unique class for this purpose.
  The class Count_distinct_field is used only by the function
  collect_statistics_for_table to calculate the values for 
  column avg_frequency of the statistical table column_stats.
*/
    
class Count_distinct_field: public Sql_alloc
{
protected:

  /* Field for which the number of distinct values is to be find out */
  Field *table_field;  
  Unique *tree;       /* The helper object to contain distinct values */
  uint tree_key_length; /* The length of the keys for the elements of 'tree */

  ulonglong distincts;
  ulonglong distincts_single_occurence;

public:
  
  Count_distinct_field() {}

  /**
    @param
    field               Field for which the number of distinct values is 
                        to be find out
    @param
    max_heap_table_size The limit for the memory used by the RB tree container
                        of the constructed Unique object 'tree' 

    @details
    The constructor sets the values of 'table_field' and 'tree_key_length',
    and then calls the 'new' operation to create a Unique object for 'tree'.
    The type of 'field' and the value max_heap_table_size of determine the set
    of the parameters to be passed to the constructor of the Unique object. 
  */  

  Count_distinct_field(Field *field, size_t max_heap_table_size)
  {
    table_field= field;
    tree_key_length= field->pack_length();

    tree= new Unique((qsort_cmp2) simple_str_key_cmp, (void*) field,
                     tree_key_length, max_heap_table_size, 1);
  }

  virtual ~Count_distinct_field()
  {
    delete tree;
    tree= NULL;
  }

  /* 
    @brief
    Check whether the Unique object tree has been successfully created
  */
  bool exists()
  {
    return (tree != NULL);
  }

  /*
    @brief
    Add the value of 'field' to the container of the Unique object 'tree'
  */
  virtual bool add()
  {
    table_field->mark_unused_memory_as_defined();
    return tree->unique_add(table_field->ptr);
  }

  /*
    @brief
    Calculate the number of elements accumulated in the container of 'tree'
  */
  void walk_tree()
  {
    Basic_stats_collector stats_collector;
    tree->walk(table_field->table, basic_stats_collector_walk,
               (void*)&stats_collector );
    distincts= stats_collector.get_count_distinct();
    distincts_single_occurence= stats_collector.get_count_single_occurence();
  }

  /*
    @brief
    Calculate a histogram of the tree
  */
  bool walk_tree_with_histogram(ha_rows rows)
  {
    Histogram_base *hist= table_field->collected_stats->histogram;
    Histogram_builder *hist_builder=
       hist->create_builder(table_field, tree_key_length, rows);

    if (tree->walk(table_field->table, histogram_build_walk,
                   (void*)hist_builder))
    {
      delete hist_builder;
      return true; // Error
    }
    hist_builder->finalize();
    distincts= hist_builder->counters.get_count_distinct();
    distincts_single_occurence= hist_builder->counters.
                                  get_count_single_occurence();
    delete hist_builder;
    return false;
  }

  ulonglong get_count_distinct()
  {
    return distincts;
  }

  ulonglong get_count_distinct_single_occurence()
  {
    return distincts_single_occurence;
  }

  /*
    @brief
    Get the pointer to the histogram built for table_field
  */
  Histogram_base *get_histogram()
  {
    return table_field->collected_stats->histogram;
  }

};


static
int simple_ulonglong_key_cmp(void* arg, uchar* key1, uchar* key2)
{
  ulonglong *val1= (ulonglong *) key1;
  ulonglong *val2= (ulonglong *) key2;
  return *val1 > *val2 ? 1 : *val1 == *val2 ? 0 : -1; 
}
  

/* 
  The class Count_distinct_field_bit is derived from the class 
  Count_distinct_field to be used only for fields of the MYSQL_TYPE_BIT type.
  The class provides a different implementation for the method add 
*/

class Count_distinct_field_bit: public Count_distinct_field
{
public:

  Count_distinct_field_bit(Field *field, size_t max_heap_table_size)
  {
    table_field= field;
    tree_key_length= sizeof(ulonglong);

    tree= new Unique((qsort_cmp2) simple_ulonglong_key_cmp,
                     (void*) &tree_key_length,
                     tree_key_length, max_heap_table_size, 1);
  }

  bool add()
  {
    longlong val= table_field->val_int();   
    return tree->unique_add(&val);
  }
};


/* 
  The class Index_prefix_calc is a helper class used to calculate the values
  for the column 'avg_frequency' of the statistical table index_stats.
  For any table t from the database db and any k-component prefix of the
  index i for this table the row from index_stats with the primary key
  (db,t,i,k) must contain in the column 'avg_frequency' either NULL or 
  the number that is the ratio of N and V, where N is the number of index
  entries without NULL values in the first k components of the index i,
  and V is the number of distinct tuples composed of the first k components
  encountered among these index entries.  
  Currently the objects of this class are used only by the function
  collect_statistics_for_index. 
*/

class Index_prefix_calc: public Sql_alloc
{

private:

  /* Table containing index specified by index_info */
  TABLE *index_table;  
  /* Info for the index i for whose prefix 'avg_frequency' is calculated */
  KEY *index_info;  
  /* The maximum number of the components in the prefixes of interest */   
  uint prefixes; 
  bool empty;  

  /* This structure is created for every k components of the index i */
  class Prefix_calc_state
  {
  public:
    /* 
      The number of the scanned index entries without nulls 
      in the first k components
    */
    ulonglong entry_count;
    /* 
      The number if the scanned index entries without nulls with 
      the last encountered k-component prefix
    */
    ulonglong prefix_count;
    /* The values of the last encountered k-component prefix */
    Cached_item *last_prefix;
  };

  /* 
    Array of structures used to calculate 'avg_frequency' for different
    prefixes of the index i
  */   
  Prefix_calc_state *calc_state;
    
public:

  bool is_single_comp_pk;
  bool is_partial_fields_present;

  Index_prefix_calc(THD *thd, TABLE *table, KEY *key_info)
    : index_table(table), index_info(key_info), prefixes(0), empty(true),
    calc_state(NULL), is_single_comp_pk(false), is_partial_fields_present(false)
  {
    uint i;
    Prefix_calc_state *state;
    uint key_parts= table->actual_n_key_parts(key_info);

    uint pk= table->s->primary_key;
    if ((uint) (table->key_info - key_info) == pk &&
        table->key_info[pk].user_defined_key_parts == 1)
    {
      prefixes= 1;
      is_single_comp_pk= TRUE;
      return;
    }
        
    if ((calc_state=
         (Prefix_calc_state *) thd->alloc(sizeof(Prefix_calc_state)*key_parts)))
    {
      uint keyno= (uint)(key_info-table->key_info);
      for (i= 0, state= calc_state; i < key_parts; i++, state++)
      {
        /* 
          Do not consider prefixes containing a component that is only part
          of the field. This limitation is set to avoid fetching data when
          calculating the values of 'avg_frequency' for prefixes.
	*/   
        if (!key_info->key_part[i].field->part_of_key.is_set(keyno))
        {
          is_partial_fields_present= TRUE;
          break;
        }

        if (!(state->last_prefix=
              new (thd->mem_root) Cached_item_field(thd,
                                    key_info->key_part[i].field)))
          break;
        state->entry_count= state->prefix_count= 0;
        prefixes++;
      }
    }
  }


  /** 
    @breif
    Change the elements of calc_state after reading the next index entry

    @details
    This function is to be called at the index scan each time the next
    index entry has been read into the record buffer.
    For each of the index prefixes the function checks whether nulls
    are encountered in any of the k components of the prefix.
    If this is not the case the value of calc_state[k-1].entry_count
    is incremented by 1. Then the function checks whether the value of
    any of these k components has changed. If so, the value of 
    calc_state[k-1].prefix_count is incremented by 1. 
  */

  void add()
  {
    uint i;
    Prefix_calc_state *state;
    uint first_changed= prefixes;
    for (i= prefixes, state= calc_state+prefixes-1; i; i--, state--)
    {
      if (state->last_prefix->cmp())
        first_changed= i-1;
    }
    if (empty)
    {
      first_changed= 0;
      empty= FALSE;
    }
    for (i= 0, state= calc_state; i < prefixes; i++, state++)
    {
      if (state->last_prefix->null_value)
        break;
      if (i >= first_changed)
        state->prefix_count++;
      state->entry_count++;
    }   
  }

  /**
    @brief
    Calculate the values of avg_frequency for all prefixes of an index

    @details
    This function is to be called after the index scan to count the number
    of distinct index prefixes has been done. The function calculates
    the value of avg_frequency for the index prefix with k components
    as calc_state[k-1].entry_count/calc_state[k-1].prefix_count.
    If calc_state[k-1].prefix_count happens to be 0, the value of
    avg_frequency[k-1] is set to 0, i.e. is considered as unknown.
  */

  void get_avg_frequency()
  {
    uint i;
    Prefix_calc_state *state;

    if (is_single_comp_pk)
    {
      index_info->collected_stats->set_avg_frequency(0, 1.0);
      return;
    }

    for (i= 0, state= calc_state; i < prefixes; i++, state++)
    {
      if (i < prefixes)
      {
        double val= state->prefix_count == 0 ?
	            0 : (double) state->entry_count / state->prefix_count;                     
        index_info->collected_stats->set_avg_frequency(i, val);
      }
    }
  }       
};


/**
  @brief 
  Create fields for min/max values to collect column statistics

  @param
  table       Table the fields are created for

  @details
  The function first allocates record buffers to store min/max values
  for 'table's fields. Then for each table field f it creates Field structures
  that points to these buffers rather that to the record buffer as the
  Field object for f does. The pointers of the created fields are placed
  in the collected_stats structure of the Field object for f.
  The function allocates the buffers for min/max values in the table
  memory. 

  @note 
  The buffers allocated when min/max values are used to read statistics
  from the persistent statistical tables differ from those buffers that
  are used when statistics on min/max values for column is collected
  as they are allocated in different mem_roots.
  The same is true for the fields created for min/max values.  
*/      

static
void create_min_max_statistical_fields_for_table(TABLE *table)
{
  uint rec_buff_length= table->s->rec_buff_length;

  if ((table->collected_stats->min_max_record_buffers=
       (uchar *) alloc_root(&table->mem_root, 2*rec_buff_length)))
  {
    uchar *record= table->collected_stats->min_max_record_buffers;
    memset(record, 0,  2*rec_buff_length);

    for (uint i=0; i < 2; i++, record+= rec_buff_length)
    {
      for (Field **field_ptr= table->field; *field_ptr; field_ptr++)
      {
        Field *fld;
        Field *table_field= *field_ptr;
        my_ptrdiff_t diff= record-table->record[0];
        if (!bitmap_is_set(table->read_set, table_field->field_index))
          continue; 
        if (!(fld= table_field->clone(&table->mem_root, table, diff)))
          continue;
        if (i == 0)
          table_field->collected_stats->min_value= fld;
        else
          table_field->collected_stats->max_value= fld;
      }
    }
  }
}


/**
  @brief 
  Create fields for min/max values to read column statistics

  @param
  thd          Thread handler
  @param
  table_share  Table share the fields are created for
  @param
  is_safe      TRUE <-> at any time only one thread can perform the function

  @details
  The function first allocates record buffers to store min/max values
  for 'table_share's fields. Then for each field f it creates Field structures
  that points to these buffers rather that to the record buffer as the
  Field object for f does. The pointers of the created fields are placed
  in the read_stats structure of the Field object for f.
  The function allocates the buffers for min/max values in the table share
  memory. 
  If the parameter is_safe is TRUE then it is guaranteed that at any given time
  only one thread is executed the code of the function.

  @note 
  The buffers allocated when min/max values are used to collect statistics
  from the persistent statistical tables differ from those buffers that
  are used when statistics on min/max values for column is read as they
  are allocated in different mem_roots.
  The same is true for the fields created for min/max values.  
*/      

static
void create_min_max_statistical_fields_for_table_share(THD *thd,
                                                       TABLE_SHARE *table_share)
{
  TABLE_STATISTICS_CB *stats_cb= &table_share->stats_cb;
  Table_statistics *stats= stats_cb->table_stats; 

  if (stats->min_max_record_buffers)
    return;
   
  uint rec_buff_length= table_share->rec_buff_length;

  if ((stats->min_max_record_buffers=
         (uchar *) alloc_root(&stats_cb->mem_root, 2*rec_buff_length)))
  {
    uchar *record= stats->min_max_record_buffers;
    memset(record, 0,  2*rec_buff_length);

    for (uint i=0; i < 2; i++, record+= rec_buff_length)
    {
      for (Field **field_ptr= table_share->field; *field_ptr; field_ptr++)
      {
        Field *fld;
        Field *table_field= *field_ptr;
        my_ptrdiff_t diff= record - table_share->default_values;
        if (!(fld= table_field->clone(&stats_cb->mem_root, NULL, diff)))
          continue;
        if (i == 0)
          table_field->read_stats->min_value= fld;
        else
          table_field->read_stats->max_value= fld;
      }
    }
  }

}


/**
  @brief 
  Allocate memory for the table's statistical data to be collected

  @param
  table       Table for which the memory for statistical data is allocated

  @note
  The function allocates the memory for the statistical data on 'table' with
  the intention to collect the data there. The memory is allocated for
  the statistics on the table, on the table's columns, and on the table's
  indexes. The memory is allocated in the table's mem_root.

  @retval
  0      If the memory for all statistical data has been successfully allocated  
  @retval
  1      Otherwise

  @note 
  Each thread allocates its own memory to collect statistics on the table
  It allows us, for example, to collect statistics on the different indexes
  of the same table in parallel. 
*/      

int alloc_statistics_for_table(THD* thd, TABLE *table)
{ 
  Field **field_ptr;

  DBUG_ENTER("alloc_statistics_for_table");

  uint columns= 0;
  for (field_ptr= table->field; *field_ptr; field_ptr++)
  {
    if (bitmap_is_set(table->read_set, (*field_ptr)->field_index))
      columns++;
  }

  Table_statistics *table_stats= 
    (Table_statistics *) alloc_root(&table->mem_root,
                                    sizeof(Table_statistics));

  Column_statistics_collected *column_stats=
    (Column_statistics_collected *) alloc_root(&table->mem_root,
                                    sizeof(Column_statistics_collected) *
				    columns);

  uint keys= table->s->keys;
  Index_statistics *index_stats=
    (Index_statistics *) alloc_root(&table->mem_root,
                                    sizeof(Index_statistics) * keys);

  uint key_parts= table->s->ext_key_parts;
  ulonglong *idx_avg_frequency= (ulonglong*) alloc_root(&table->mem_root,
                                               sizeof(ulonglong) * key_parts);

  if (!table_stats || !column_stats || !index_stats || !idx_avg_frequency)
    DBUG_RETURN(1);

  table->collected_stats= table_stats;
  table_stats->column_stats= column_stats;
  table_stats->index_stats= index_stats;
  table_stats->idx_avg_frequency= idx_avg_frequency;
  
  memset(column_stats, 0, sizeof(Column_statistics) * columns);

  for (field_ptr= table->field; *field_ptr; field_ptr++)
  {
    if (bitmap_is_set(table->read_set, (*field_ptr)->field_index))
    {
      column_stats->histogram = NULL;
      (*field_ptr)->collected_stats= column_stats++;
    }
  }

  memset(idx_avg_frequency, 0, sizeof(ulonglong) * key_parts);

  KEY *key_info, *end;
  for (key_info= table->key_info, end= key_info + table->s->keys;
       key_info < end; 
       key_info++, index_stats++)
  {
    key_info->collected_stats= index_stats;
    key_info->collected_stats->init_avg_frequency(idx_avg_frequency);
    idx_avg_frequency+= key_info->ext_key_parts;
  }

  create_min_max_statistical_fields_for_table(table);

  DBUG_RETURN(0);
}

/*
  Free the "local" statistics for table.
  We only free the statistics that is not on MEM_ROOT and needs to be
  explicitly freed.
*/
void free_statistics_for_table(THD *thd, TABLE *table)
{
  for (Field **field_ptr= table->field; *field_ptr; field_ptr++)
  {
    // Only delete the histograms that are exclusivly owned by this thread
    if ((*field_ptr)->collected_stats &&
        (*field_ptr)->collected_stats->histogram &&
        (*field_ptr)->collected_stats->histogram->get_owner() == thd)
    {
      delete (*field_ptr)->collected_stats->histogram;
      (*field_ptr)->collected_stats->histogram= NULL;
    }
  }
}

/**
  @brief 
  Allocate memory for the statistical data used by a table share

  @param
  thd         Thread handler
  @param
  table_share Table share for which the memory for statistical data is allocated

  @note
  The function allocates the memory for the statistical data on a table in the
  table's share memory with the intention to read the statistics there from
  the system persistent statistical tables mysql.table_stat, mysql.column_stats,
  mysql.index_stats. The memory is allocated for the statistics on the table,
  on the tables's columns, and on the table's indexes. The memory is allocated
  in the table_share's mem_root.

  @retval
  0     If the memory for all statistical data has been successfully allocated  
  @retval
  1     Otherwise

  @note
  The situation when more than one thread try to allocate memory for 
  statistical data is rare. It happens under the following scenario:
  1. One thread executes a query over table t with the system variable 
    'use_stat_tables' set to 'never'.
  2. After this the second thread sets 'use_stat_tables' to 'preferably'
     and executes a query over table t.    
  3. Simultaneously the third thread sets 'use_stat_tables' to 'preferably'
     and executes a query over table t. 
  Here the second and the third threads try to allocate the memory for
  statistical data at the same time. The precautions are taken to
  guarantee the correctness of the allocation.
*/      

static int alloc_statistics_for_table_share(THD* thd, TABLE_SHARE *table_share)
{
  Field **field_ptr;
  KEY *key_info, *end;
  TABLE_STATISTICS_CB *stats_cb= &table_share->stats_cb;

  DBUG_ENTER("alloc_statistics_for_table_share");

  Table_statistics *table_stats= stats_cb->table_stats;
  if (!table_stats)
  {
    table_stats=  (Table_statistics *) alloc_root(&stats_cb->mem_root,
                                                  sizeof(Table_statistics));
    if (!table_stats)
      DBUG_RETURN(1);
    memset(table_stats, 0, sizeof(Table_statistics));
    stats_cb->table_stats= table_stats;
  }

  uint fields= table_share->fields;
  Column_statistics *column_stats= table_stats->column_stats;
  if (!column_stats)
  {
    column_stats= (Column_statistics *) alloc_root(&stats_cb->mem_root,
                                                   sizeof(Column_statistics) *
				                   (fields+1));  
    if (column_stats)
    { 
      memset(column_stats, 0, sizeof(Column_statistics) * (fields+1));
      table_stats->column_stats= column_stats;
      for (field_ptr= table_share->field;
           *field_ptr;
           field_ptr++, column_stats++)
      {
        (*field_ptr)->read_stats= column_stats;
        (*field_ptr)->read_stats->min_value= NULL;
        (*field_ptr)->read_stats->max_value= NULL;
      }
      create_min_max_statistical_fields_for_table_share(thd, table_share);
    }
  }

  uint keys= table_share->keys;
  Index_statistics *index_stats= table_stats->index_stats;
  if (!index_stats)
  {
    index_stats= (Index_statistics *) alloc_root(&stats_cb->mem_root,
                                                 sizeof(Index_statistics) *
                                                 keys);
    if (index_stats)
    {
      table_stats->index_stats= index_stats;   
      for (key_info= table_share->key_info, end= key_info + keys;
           key_info < end; 
           key_info++, index_stats++)
      {
        key_info->read_stats= index_stats;
      }
    }   
  }

  uint key_parts= table_share->ext_key_parts;
  ulonglong *idx_avg_frequency=  table_stats->idx_avg_frequency;
  if (!idx_avg_frequency)
  {
    idx_avg_frequency= (ulonglong*) alloc_root(&stats_cb->mem_root,
                                               sizeof(ulonglong) * key_parts);
    if (idx_avg_frequency)
    {
      memset(idx_avg_frequency, 0, sizeof(ulonglong) * key_parts);
      table_stats->idx_avg_frequency= idx_avg_frequency;
      for (key_info= table_share->key_info, end= key_info + keys;
           key_info < end; 
           key_info++)
      {
        key_info->read_stats->init_avg_frequency(idx_avg_frequency);
        idx_avg_frequency+= key_info->ext_key_parts;
      }
    }   
  }
  DBUG_RETURN(column_stats && index_stats && idx_avg_frequency ? 0 : 1);
}


/**
  @brief
  Initialize the aggregation fields to collect statistics on a column

  @param
  thd            Thread handler
  @param
  table_field    Column to collect statistics for
*/

inline
void Column_statistics_collected::init(THD *thd, Field *table_field)
{
  size_t max_heap_table_size= (size_t)thd->variables.max_heap_table_size;
  TABLE *table= table_field->table;
  uint pk= table->s->primary_key;
  
  is_single_pk_col= FALSE;

  if (pk != MAX_KEY && table->key_info[pk].user_defined_key_parts == 1 &&
      table->key_info[pk].key_part[0].fieldnr == table_field->field_index + 1)
    is_single_pk_col= TRUE;  
  
  column= table_field;

  set_all_nulls();

  nulls= 0;
  column_total_length= 0;
  if (is_single_pk_col)
    count_distinct= NULL;
  if (table_field->flags & BLOB_FLAG)
    count_distinct= NULL;
  else
  {
    count_distinct=
      table_field->type() == MYSQL_TYPE_BIT ?
      new Count_distinct_field_bit(table_field, max_heap_table_size) :
      new Count_distinct_field(table_field, max_heap_table_size);
  }
  if (count_distinct && !count_distinct->exists())
    count_distinct= NULL;
}


/**
  @brief
  Perform aggregation for a row when collecting statistics on a column

  @param
  rowno     The order number of the row
*/

inline
bool Column_statistics_collected::add()
{

  bool err= 0;
  if (column->is_null())
    nulls++;
  else
  {
    column_total_length+= column->value_length();
    if (min_value && column->update_min(min_value,
                                        is_null(COLUMN_STAT_MIN_VALUE)))
      set_not_null(COLUMN_STAT_MIN_VALUE);
    if (max_value && column->update_max(max_value,
                                        is_null(COLUMN_STAT_MAX_VALUE)))
      set_not_null(COLUMN_STAT_MAX_VALUE);
    if (count_distinct)
      err= count_distinct->add();
  } 
  return err;
}


/**
  @brief
  Get the results of aggregation when collecting the statistics on a column
  
  @param
  rows          The total number of rows in the table 
*/

inline
bool Column_statistics_collected::finish(MEM_ROOT *mem_root, ha_rows rows,
                                         double sample_fraction)
{
  double val;

  if (rows)
  {
     val= (double) nulls / rows;
     set_nulls_ratio(val);
     set_not_null(COLUMN_STAT_NULLS_RATIO);
  }
  if (rows - nulls)
  {
     val= (double) column_total_length / (rows - nulls);
     set_avg_length(val);
     set_not_null(COLUMN_STAT_AVG_LENGTH);
  }
  if (count_distinct)
  {
    uint hist_size= current_thd->variables.histogram_size;
    Histogram_type hist_type= 
      (Histogram_type) (current_thd->variables.histogram_type);
    bool have_histogram= false;
    if (hist_size != 0 && hist_type != INVALID_HISTOGRAM)
    {
      have_histogram= true;
      histogram= create_histogram(mem_root, hist_type, current_thd);
      histogram->init_for_collection(mem_root, hist_type, hist_size);
    }

    /* Compute cardinality statistics and optionally histogram. */
    if (!have_histogram)
      count_distinct->walk_tree();
    else
    {
      if (count_distinct->walk_tree_with_histogram(rows - nulls))
      {
        delete histogram;
        histogram= NULL;

        delete count_distinct;
        count_distinct= NULL;
        return true; // Error
      }
    }

    ulonglong distincts= count_distinct->get_count_distinct();
    ulonglong distincts_single_occurence=
      count_distinct->get_count_distinct_single_occurence();

    if (distincts)
    {
      /*
       We use the unsmoothed first-order jackknife estimator" to estimate
       the number of distinct values.
       With a sufficient large percentage of rows sampled (80%), we revert back
       to computing the avg_frequency off of the raw data.
      */
      if (sample_fraction > 0.8)
        val= (double) (rows - nulls) / distincts;
      else
      {
        if (nulls == 1)
          distincts_single_occurence+= 1;
        if (nulls)
          distincts+= 1;
        double fraction_single_occurence=
          static_cast<double>(distincts_single_occurence) / rows;
        double total_number_of_rows= rows / sample_fraction;
        double estimate_total_distincts= total_number_of_rows /
                (distincts /
                 (1.0 - (1.0 - sample_fraction) * fraction_single_occurence));
        val = std::fmax(estimate_total_distincts * (rows - nulls) / rows, 1.0);
      }

      set_avg_frequency(val);
      set_not_null(COLUMN_STAT_AVG_FREQUENCY);
    }
    else
      have_histogram= false;

    set_not_null(COLUMN_STAT_HIST_SIZE);
    if (have_histogram && distincts && histogram)
    {
      set_not_null(COLUMN_STAT_HIST_TYPE);
      set_not_null(COLUMN_STAT_HISTOGRAM);
    }
    delete count_distinct;
    count_distinct= NULL;
  }
  else if (is_single_pk_col)
  {
    val= 1.0;
    set_avg_frequency(val); 
    set_not_null(COLUMN_STAT_AVG_FREQUENCY);
  }
  return false;
}


/**
  @brief
  Clean up auxiliary structures used for aggregation
*/

inline
void Column_statistics_collected::cleanup()
{
  if (count_distinct)
  { 
    delete count_distinct;
    count_distinct= NULL;
  }
}


/**
  @brief
  Collect statistical data on an index

  @param 
  table       The table the index belongs to
  index       The number of this index in the table

  @details
  The function collects the value of 'avg_frequency' for the prefixes
  on an index from 'table'. The index is specified by its number.
  If the scan is successful the calculated statistics is saved in the
  elements of the array write_stat.avg_frequency of the KEY_INFO structure
  for the index. The statistics for the prefix with k components is saved
  in the element number k-1.

  @retval
  0         If the statistics has been successfully collected  
  @retval
  1         Otherwise

  @note
  The function collects statistics for the index prefixes for one index
  scan during which no data is fetched from the table records. That's why
  statistical data for prefixes that contain part of a field is not
  collected.
  The function employs an object of the helper class Index_prefix_calc to
  count for each index prefix the number of index entries without nulls and
  the number of distinct entries among them.
 
*/

static
int collect_statistics_for_index(THD *thd, TABLE *table, uint index)
{
  int rc= 0;
  KEY *key_info= &table->key_info[index];
  ha_rows rows= 0;

  DBUG_ENTER("collect_statistics_for_index");

  /* No statistics for FULLTEXT indexes. */
  if (key_info->flags & (HA_FULLTEXT|HA_SPATIAL))
    DBUG_RETURN(rc);

  Index_prefix_calc index_prefix_calc(thd, table, key_info);

  DEBUG_SYNC(table->in_use, "statistics_collection_start1");
  DEBUG_SYNC(table->in_use, "statistics_collection_start2");

  if (index_prefix_calc.is_single_comp_pk)
  {
    index_prefix_calc.get_avg_frequency();
    DBUG_RETURN(rc);
  }

  /*
    Request "only index read" in case of absence of fields which are
    partially in the index to avoid problems with partitioning (for example)
    which want to get whole field value.
  */
  if (!index_prefix_calc.is_partial_fields_present)
    table->file->ha_start_keyread(index);
  table->file->ha_index_init(index, TRUE);
  rc= table->file->ha_index_first(table->record[0]);
  while (rc != HA_ERR_END_OF_FILE)
  {
    if (thd->killed)
      break;

    if (rc)
      break;
    rows++;
    index_prefix_calc.add();
    rc= table->file->ha_index_next(table->record[0]);
  }
  table->file->ha_end_keyread();
  table->file->ha_index_end();

  rc= (rc == HA_ERR_END_OF_FILE && !thd->killed) ? 0 : 1;

  if (!rc)
    index_prefix_calc.get_avg_frequency();

  DBUG_RETURN(rc);
}


/**
  @brief 
  Collect statistical data for a table

  @param
  thd         The thread handle
  @param
  table       The table to collect statistics on

  @details
  The function collects data for various statistical characteristics on
  the table 'table'. These data is saved in the internal fields that could
  be reached from 'table'. The data is prepared to be saved in the persistent
  statistical table by the function update_statistics_for_table.
  The collected statistical values are not placed in the same fields that
  keep the statistical data used by the optimizer. Therefore, at any time,
  there is no collision between the statistics being collected and the one
  used by the optimizer to look for optimal query execution plans for other
  clients.

  @retval
  0         If the statistics has been successfully collected  
  @retval
  1         Otherwise

  @note
  The function first collects statistical data for statistical characteristics
  to be saved in the statistical tables table_stat and column_stats. To do this
  it performs a full table scan of 'table'. At this scan the function collects
  statistics on each column of the table and count the total number of the
  scanned rows. To calculate the value of 'avg_frequency' for a column the
  function constructs an object of the helper class Count_distinct_field
  (or its derivation). Currently this class cannot count the number of
  distinct values for blob columns. So the value of 'avg_frequency' for
  blob columns is always null.
  After the full table scan the function calls collect_statistics_for_index
  for each table index. The latter performs full index scan for each index.

  @note
  Currently the statistical data is collected indiscriminately for all
  columns/indexes of 'table', for all statistical characteristics.
  TODO. Collect only specified statistical characteristics for specified
  columns/indexes.

  @note
  Currently the process of collecting statistical data is not optimized.
  For example, 'avg_frequency' for a column could be copied from the
  'avg_frequency' collected for an index if this column is used as the
  first component of the index. Min and min values for this column could
  be extracted from the index as well.       
*/

int collect_statistics_for_table(THD *thd, TABLE *table)
{
  int rc;
  Field **field_ptr;
  Field *table_field;
  ha_rows rows= 0;
  handler *file=table->file;
  double sample_fraction= thd->variables.sample_percentage / 100;
  const ha_rows MIN_THRESHOLD_FOR_SAMPLING= 50000;

  DBUG_ENTER("collect_statistics_for_table");

  table->collected_stats->cardinality_is_null= TRUE;
  table->collected_stats->cardinality= 0;

  if (thd->variables.sample_percentage == 0)
  {
    if (file->records() < MIN_THRESHOLD_FOR_SAMPLING)
    {
      sample_fraction= 1;
    }
    else
    {
      sample_fraction= std::fmin(
                  (MIN_THRESHOLD_FOR_SAMPLING + 4096 *
                   log(200 * file->records())) / file->records(), 1);
    }
  }

  for (field_ptr= table->field; *field_ptr; field_ptr++)
  {
    table_field= *field_ptr;   
    if (!table_field->collected_stats)
      continue; 
    table_field->collected_stats->init(thd, table_field);
  }

  restore_record(table, s->default_values);

  /* Perform a full table scan to collect statistics on 'table's columns */
  if (!(rc= file->ha_rnd_init(TRUE)))
  {
    DEBUG_SYNC(table->in_use, "statistics_collection_start");

    while ((rc= file->ha_rnd_next(table->record[0])) != HA_ERR_END_OF_FILE)
    {
      if (thd->killed)
        break;

      if (rc)
        break;

      if (thd_rnd(thd) <= sample_fraction)
      {
        for (field_ptr= table->field; *field_ptr; field_ptr++)
        {
          table_field= *field_ptr;
          if (!table_field->collected_stats)
            continue;
          if ((rc= table_field->collected_stats->add()))
            break;
        }
        if (rc)
          break;
        rows++;
      }
    }
    file->ha_rnd_end();
  }
  rc= (rc == HA_ERR_END_OF_FILE && !thd->killed) ? 0 : 1;

  /* 
    Calculate values for all statistical characteristics on columns and
    and for each field f of 'table' save them in the write_stat structure
    from the Field object for f. 
  */
  if (!rc)
  {
    table->collected_stats->cardinality_is_null= FALSE;
    table->collected_stats->cardinality=
      static_cast<ha_rows>(rows / sample_fraction);
  }

  bitmap_clear_all(table->write_set);
  for (field_ptr= table->field; *field_ptr; field_ptr++)
  {
    table_field= *field_ptr;
    if (!table_field->collected_stats)
      continue;
    bitmap_set_bit(table->write_set, table_field->field_index); 
    if (!rc)
    {
      rc= table_field->collected_stats->finish(&table->mem_root, rows,
                                               sample_fraction);
    }
    else
      table_field->collected_stats->cleanup();
  }
  bitmap_clear_all(table->write_set);

  if (!rc)
  {
    uint key;
    key_map::Iterator it(table->keys_in_use_for_query);

    MY_BITMAP *save_read_set= table->read_set;
    table->read_set= &table->tmp_set;
    bitmap_set_all(table->read_set);
     
    /* Collect statistics for indexes */
    while ((key= it++) != key_map::Iterator::BITMAP_END)
    {
      if ((rc= collect_statistics_for_index(thd, table, key)))
        break;
    }

    table->read_set= save_read_set;
  }

  DBUG_RETURN(rc);          
}


/**
  @brief
  Update statistics for a table in the persistent statistical tables

  @param
  thd         The thread handle
  @param
  table       The table to collect statistics on

  @details
  For each statistical table st the function looks for the rows from this
  table that contain statistical data on 'table'. If rows with given 
  statistical characteristics exist they are updated with the new statistical
  values taken from internal structures for 'table'. Otherwise new rows
  with these statistical characteristics are added into st.
  It is assumed that values stored in the statistical tables are found and
  saved by the function collect_statistics_for_table. 

  @retval
  0         If all statistical tables has been successfully updated  
  @retval
  1         Otherwise

  @note
  The function is called when executing the ANALYZE actions for 'table'.
  The function first unlocks the opened table the statistics on which has
  been collected, but does not closes it, so all collected statistical data
  remains in internal structures for 'table'. Then the function opens the
  statistical tables and writes the statistical data for 'table'into them.
  It is not allowed just to open statistical tables for writing when some
  other tables are locked for reading.
  After the statistical tables have been opened they are updated one by one
  with the new statistics on 'table'. Objects of the helper classes
  Table_stat, Column_stat and Index_stat are employed for this. 
  After having been updated the statistical system tables are closed.     
*/

int update_statistics_for_table(THD *thd, TABLE *table)
{
  TABLE_LIST tables[STATISTICS_TABLES];
  uint i;
  int err;
  enum_binlog_format save_binlog_format;
  int rc= 0;
  TABLE *stat_table;
  DBUG_ENTER("update_statistics_for_table");

  DEBUG_SYNC(thd, "statistics_update_start");

  start_new_trans new_trans(thd);

  if ((open_stat_tables(thd, tables, TRUE)))
    DBUG_RETURN(rc);
   
  save_binlog_format= thd->set_current_stmt_binlog_format_stmt();

  /* Update the statistical table table_stats */
  stat_table= tables[TABLE_STAT].table;
  Table_stat table_stat(stat_table, table);
  restore_record(stat_table, s->default_values);
  table_stat.set_key_fields();
  err= table_stat.update_stat();
  if (err)
    rc= 1;

  /* Update the statistical table colum_stats */
  stat_table= tables[COLUMN_STAT].table;
  Column_stat column_stat(stat_table, table);
  for (Field **field_ptr= table->field; *field_ptr; field_ptr++)
  {
    Field *table_field= *field_ptr;
    if (!table_field->collected_stats)
      continue;
    restore_record(stat_table, s->default_values);
    column_stat.set_key_fields(table_field);
    err= column_stat.update_stat();
    if (err && !rc)
      rc= 1;
  }

  /* Update the statistical table index_stats */
  stat_table= tables[INDEX_STAT].table;
  uint key;
  key_map::Iterator it(table->keys_in_use_for_query);
  Index_stat index_stat(stat_table, table);

  while ((key= it++) != key_map::Iterator::BITMAP_END)
  {
    KEY *key_info= table->key_info+key;
    uint key_parts= table->actual_n_key_parts(key_info);
    for (i= 0; i < key_parts; i++)
    {
      restore_record(stat_table, s->default_values);
      index_stat.set_key_fields(key_info, i+1);
      err= index_stat.update_stat();
      if (err && !rc)
        rc= 1;
    }
  }

  thd->restore_stmt_binlog_format(save_binlog_format);
  if (thd->commit_whole_transaction_and_close_tables())
    rc= 1;
  new_trans.restore_old_transaction();

  DBUG_RETURN(rc);
}


/**
  @brief
  Read statistics for a table from the persistent statistical tables

  @param
  thd         The thread handle
  @param
  table       The table to read statistics on
  @param
  stat_tables The array of TABLE_LIST objects for statistical tables

  @details
  For each statistical table the function looks for the rows from this
  table that contain statistical data on 'table'. If such rows is found
  the data from statistical columns of it is read into the appropriate
  fields of internal structures for 'table'. Later at the query processing
  this data are supposed to be used by the optimizer. 
  The parameter stat_tables should point to an array of TABLE_LIST
  objects for all statistical tables linked into a list. All statistical
  tables are supposed to be opened.  
  The function is called by read_statistics_for_tables_if_needed().

  @retval
  0         If data has been successfully read for the table  
  @retval
  1         Otherwise

  @note
  Objects of the helper classes Table_stat, Column_stat and Index_stat
  are employed to read statistical data from the statistical tables. 
  now.        
*/

static
int read_statistics_for_table(THD *thd, TABLE *table, TABLE_LIST *stat_tables)
{
  uint i;
  TABLE *stat_table;
  Field *table_field;
  Field **field_ptr;
  KEY *key_info, *key_info_end;
  TABLE_SHARE *table_share= table->s;

  DBUG_ENTER("read_statistics_for_table");
  DEBUG_SYNC(thd, "statistics_mem_alloc_start1");
  DEBUG_SYNC(thd, "statistics_mem_alloc_start2");

  if (!table_share->stats_cb.start_stats_load())
    DBUG_RETURN(table_share->stats_cb.stats_are_ready() ? 0 : 1);

  if (alloc_statistics_for_table_share(thd, table_share))
  {
    table_share->stats_cb.abort_stats_load();
    DBUG_RETURN(1);
  }

  /* Don't write warnings for internal field conversions */
  Check_level_instant_set check_level_save(thd, CHECK_FIELD_IGNORE);

  /* Read statistics from the statistical table table_stats */
  Table_statistics *read_stats= table_share->stats_cb.table_stats;
  stat_table= stat_tables[TABLE_STAT].table;
  Table_stat table_stat(stat_table, table);
  table_stat.set_key_fields();
  table_stat.get_stat_values();
   
  /* Read statistics from the statistical table column_stats */
  stat_table= stat_tables[COLUMN_STAT].table;
  bool have_histograms= false;
  Column_stat column_stat(stat_table, table);
  for (field_ptr= table_share->field; *field_ptr; field_ptr++)
  {
    table_field= *field_ptr;
    column_stat.set_key_fields(table_field);
    column_stat.get_stat_values();
    if (table_field->read_stats->histogram_type_on_disk != INVALID_HISTOGRAM)
      have_histograms= true;
  }
  table_share->stats_cb.have_histograms= have_histograms;

  /* Read statistics from the statistical table index_stats */
  stat_table= stat_tables[INDEX_STAT].table;
  Index_stat index_stat(stat_table, table);
  for (key_info= table_share->key_info,
       key_info_end= key_info + table_share->keys;
       key_info < key_info_end; key_info++)
  {
    uint key_parts= key_info->ext_key_parts;
    for (i= 0; i < key_parts; i++)
    {
      index_stat.set_key_fields(key_info, i+1);
      index_stat.get_stat_values();
    }
   
    key_part_map ext_key_part_map= key_info->ext_key_part_map;
    if (key_info->user_defined_key_parts != key_info->ext_key_parts &&
        key_info->read_stats->get_avg_frequency(key_info->user_defined_key_parts) == 0)
    {
      KEY *pk_key_info= table_share->key_info + table_share->primary_key;
      uint k= key_info->user_defined_key_parts;
      uint pk_parts= pk_key_info->user_defined_key_parts;
      ha_rows n_rows= read_stats->cardinality;
      double k_dist= n_rows / key_info->read_stats->get_avg_frequency(k-1);
      uint m= 0;
      for (uint j= 0; j < pk_parts; j++)
      {
        if (!(ext_key_part_map & 1 << j))
	{
          for (uint l= k; l < k + m; l++)
	  {
            double avg_frequency=
                     pk_key_info->read_stats->get_avg_frequency(j-1);
            set_if_smaller(avg_frequency, 1);
            double val= pk_key_info->read_stats->get_avg_frequency(j) /
	                avg_frequency; 
	    key_info->read_stats->set_avg_frequency (l, val);
          }
        }
        else
	{
	  double avg_frequency= pk_key_info->read_stats->get_avg_frequency(j);
	  key_info->read_stats->set_avg_frequency(k + m, avg_frequency);
	  m++;
        }    
      }      
      for (uint l= k; l < k + m; l++)
      {
        double avg_frequency= key_info->read_stats->get_avg_frequency(l);
        if (avg_frequency == 0 || read_stats->cardinality_is_null)
          avg_frequency= 1;
        else if (avg_frequency > 1)
	{
          avg_frequency/= k_dist;
          set_if_bigger(avg_frequency, 1);
	}
        key_info->read_stats->set_avg_frequency(l, avg_frequency);
      }
    }
  }

  table_share->stats_cb.end_stats_load();
  DBUG_RETURN(0);
}


/**
  @breif
  Cleanup of min/max statistical values for table share
*/

void delete_stat_values_for_table_share(TABLE_SHARE *table_share)
{
  TABLE_STATISTICS_CB *stats_cb= &table_share->stats_cb;
  Table_statistics *table_stats= stats_cb->table_stats;
  if (!table_stats)
    return;
  Column_statistics *column_stats= table_stats->column_stats;
  if (!column_stats)
    return;

  for (Field **field_ptr= table_share->field;
       *field_ptr;
       field_ptr++, column_stats++)
  {
    if (column_stats->min_value)
    {
      delete column_stats->min_value;
      column_stats->min_value= NULL;
    }
    if (column_stats->max_value)
    {
      delete column_stats->max_value;
      column_stats->max_value= NULL;
    }

    delete column_stats->histogram;
    column_stats->histogram=NULL;
  }
}


/**
  @brief
  Read histogram for a table from the persistent statistical tables

  @param
  thd         The thread handle
  @param
  table       The table to read histograms for
  @param
  stat_tables The array of TABLE_LIST objects for statistical tables

  @details
  For the statistical table columns_stats the function looks for the rows
  from this table that contain statistical data on 'table'. If such rows
  are found the histograms from them are read into the memory allocated
  for histograms of 'table'. Later at the query processing these histogram
  are supposed to be used by the optimizer. 
  The parameter stat_tables should point to an array of TABLE_LIST
  objects for all statistical tables linked into a list. All statistical
  tables are supposed to be opened.  
  The function is called by read_statistics_for_tables_if_needed().

  @retval
  0         If data has been successfully read for the table  
  @retval
  1         Otherwise

  @note
  Objects of the helper Column_stat are employed read histogram
  from the statistical table column_stats now.        
*/

static
int read_histograms_for_table(THD *thd, TABLE *table, TABLE_LIST *stat_tables)
{
  TABLE_STATISTICS_CB *stats_cb= &table->s->stats_cb;
  DBUG_ENTER("read_histograms_for_table");

  if (stats_cb->start_histograms_load())
  {
    Column_stat column_stat(stat_tables[COLUMN_STAT].table, table);

    /*
      The process of histogram loading makes use of the field it is for. Mark
      all fields as readable/writable in order to allow that.
    */
    MY_BITMAP *old_sets[2];
    dbug_tmp_use_all_columns(table, old_sets, &table->read_set, &table->write_set);

    for (Field **field_ptr= table->s->field; *field_ptr; field_ptr++)
    {
      Field *table_field= *field_ptr;
      if (table_field->read_stats->histogram_type_on_disk != INVALID_HISTOGRAM)
      {
        column_stat.set_key_fields(table_field);
        table_field->read_stats->histogram=
          column_stat.load_histogram(&stats_cb->mem_root);
      }
    }
    stats_cb->end_histograms_load();

    dbug_tmp_restore_column_maps(&table->read_set, &table->write_set, old_sets);
  }
  table->histograms_are_read= true;
  DBUG_RETURN(0);
}

/**
  @brief
  Read statistics for tables from a table list if it is needed

  @param
  thd         The thread handle
  @param
  tables      The tables list for whose tables to read statistics

  @details
  The function first checks whether for any of the tables opened and locked
  for a statement statistics from statistical tables is needed to be read.
  Then, if so, it opens system statistical tables for read and reads
  the statistical data from them for those tables from the list for which it
  makes sense. Then the function closes system statistical tables.

  @retval
  0       Statistics for tables was successfully read  
  @retval
  1       Otherwise
*/

int read_statistics_for_tables_if_needed(THD *thd, TABLE_LIST *tables)
{
  switch (thd->lex->sql_command) {
  case SQLCOM_SELECT:
  case SQLCOM_INSERT:
  case SQLCOM_INSERT_SELECT:
  case SQLCOM_UPDATE:
  case SQLCOM_UPDATE_MULTI:
  case SQLCOM_DELETE:
  case SQLCOM_DELETE_MULTI:
  case SQLCOM_REPLACE:
  case SQLCOM_REPLACE_SELECT:
  case SQLCOM_CREATE_TABLE:
  case SQLCOM_SET_OPTION:
  case SQLCOM_DO:
    return read_statistics_for_tables(thd, tables);
  default:
    return 0;
  }
}


static void dump_stats_from_share_to_table(TABLE *table)
{
  TABLE_SHARE *table_share= table->s;
  KEY *key_info= table_share->key_info;
  KEY *key_info_end= key_info + table_share->keys;
  KEY *table_key_info= table->key_info;
  for ( ; key_info < key_info_end; key_info++, table_key_info++)
    table_key_info->read_stats= key_info->read_stats;

  Field **field_ptr= table_share->field;
  Field **table_field_ptr= table->field;
  for ( ; *field_ptr; field_ptr++, table_field_ptr++)
    (*table_field_ptr)->read_stats= (*field_ptr)->read_stats;
  table->stats_is_read= true;
}


int read_statistics_for_tables(THD *thd, TABLE_LIST *tables)
{
  TABLE_LIST stat_tables[STATISTICS_TABLES];

  DBUG_ENTER("read_statistics_for_tables");

  if (thd->bootstrap || thd->variables.use_stat_tables == NEVER)
    DBUG_RETURN(0);

  bool found_stat_table= false;
  bool statistics_for_tables_is_needed= false;

  for (TABLE_LIST *tl= tables; tl; tl= tl->next_global)
  {
    TABLE_SHARE *table_share;
    if (!tl->is_view_or_derived() && tl->table && (table_share= tl->table->s) &&
        table_share->tmp_table == NO_TMP_TABLE)
    {
      if (table_share->table_category == TABLE_CATEGORY_USER)
      {
        if (table_share->stats_cb.stats_are_ready())
        {
          if (!tl->table->stats_is_read)
            dump_stats_from_share_to_table(tl->table);
          tl->table->histograms_are_read=
            table_share->stats_cb.histograms_are_ready();
          if (table_share->stats_cb.histograms_are_ready() ||
              thd->variables.optimizer_use_condition_selectivity <= 3)
            continue;
        }
        statistics_for_tables_is_needed= true;
      }
      else if (is_stat_table(&tl->db, &tl->alias))
        found_stat_table= true;
    }
  }

  DEBUG_SYNC(thd, "statistics_read_start");

  /*
    Do not read statistics for any query that explicity involves
    statistical tables, failure to to do so we may end up
    in a deadlock.
  */
  if (found_stat_table || !statistics_for_tables_is_needed)
    DBUG_RETURN(0);

  start_new_trans new_trans(thd);

  if (open_stat_tables(thd, stat_tables, FALSE))
    DBUG_RETURN(1);

  for (TABLE_LIST *tl= tables; tl; tl= tl->next_global)
  {
    TABLE_SHARE *table_share;
    if (!tl->is_view_or_derived() && tl->table && (table_share= tl->table->s) &&
        table_share->tmp_table == NO_TMP_TABLE &&
        table_share->table_category == TABLE_CATEGORY_USER)
    {
      if (!tl->table->stats_is_read)
      {
        if (!read_statistics_for_table(thd, tl->table, stat_tables))
          dump_stats_from_share_to_table(tl->table);
        else
          continue;
      }
      if (thd->variables.optimizer_use_condition_selectivity > 3)
        (void) read_histograms_for_table(thd, tl->table, stat_tables);
    }
  }

  thd->commit_whole_transaction_and_close_tables();
  new_trans.restore_old_transaction();

  DBUG_RETURN(0);
}


/**
  @brief
  Delete statistics on a table from all statistical tables

  @param
  thd         The thread handle
  @param
  db          The name of the database the table belongs to
  @param
  tab         The name of the table whose statistics is to be deleted

  @details
  The function delete statistics on the table called 'tab' of the database
  'db' from all statistical tables: table_stats, column_stats, index_stats.

  @retval
  0         If all deletions are successful or we couldn't open statistics table
  @retval
  1         Otherwise

  @note
  The function is called when executing the statement DROP TABLE 'tab'.
*/

int delete_statistics_for_table(THD *thd, const LEX_CSTRING *db,
                                const LEX_CSTRING *tab)
{
  int err;
  enum_binlog_format save_binlog_format;
  TABLE *stat_table;
  TABLE_LIST tables[STATISTICS_TABLES];
  Open_tables_backup open_tables_backup;
  int rc= 0;
  DBUG_ENTER("delete_statistics_for_table");

  start_new_trans new_trans(thd);
   
  if (open_stat_tables(thd, tables, TRUE))
    DBUG_RETURN(0);

  save_binlog_format= thd->set_current_stmt_binlog_format_stmt();

  /* Delete statistics on table from the statistical table index_stats */
  stat_table= tables[INDEX_STAT].table;
  Index_stat index_stat(stat_table, db, tab);
  index_stat.set_full_table_name();
  while (index_stat.find_next_stat_for_prefix(2))
  {
    err= index_stat.delete_stat();
    if (err & !rc)
      rc= 1;
  }

  /* Delete statistics on table from the statistical table column_stats */
  stat_table= tables[COLUMN_STAT].table;
  Column_stat column_stat(stat_table, db, tab);
  column_stat.set_full_table_name();
  while (column_stat.find_next_stat_for_prefix(2))
  {
    err= column_stat.delete_stat();
    if (err & !rc)
      rc= 1;
  }
   
  /* Delete statistics on table from the statistical table table_stats */
  stat_table= tables[TABLE_STAT].table;
  Table_stat table_stat(stat_table, db, tab);
  table_stat.set_key_fields();
  if (table_stat.find_stat())
  {
    err= table_stat.delete_stat();
    if (err & !rc)
      rc= 1;
  }

  err= del_global_table_stat(thd, db, tab);
  if (err & !rc)
      rc= 1;

  thd->restore_stmt_binlog_format(save_binlog_format);
  thd->commit_whole_transaction_and_close_tables();
  new_trans.restore_old_transaction();

  DBUG_RETURN(rc);
}


/**
  @brief
  Delete statistics on a column of the specified table

  @param thd         The thread handle
  @param tab         The table the column belongs to
  @param col         The field of the column whose statistics is to be deleted

  @details
  The function delete statistics on the column 'col' belonging to the table 
  'tab' from the statistical table column_stats. 

  @retval 0  If all deletions are successful or we couldn't open statistics table
  @retval 1  Otherwise

  @note
  The function is called when dropping a table column  or when changing
  the definition of this column.
*/

int delete_statistics_for_column(THD *thd, TABLE *tab, Field *col)
{
  int err;
  enum_binlog_format save_binlog_format;
  TABLE *stat_table;
  TABLE_LIST tables;
  int rc= 0;
  DBUG_ENTER("delete_statistics_for_column");
   
  start_new_trans new_trans(thd);

  if (open_stat_table_for_ddl(thd, &tables, &stat_table_name[1]))
    DBUG_RETURN(0);

  save_binlog_format= thd->set_current_stmt_binlog_format_stmt();

  stat_table= tables.table;
  Column_stat column_stat(stat_table, tab);
  column_stat.set_key_fields(col);
  if (column_stat.find_stat())
  {
    err= column_stat.delete_stat();
    if (err)
      rc= 1;
  }

  thd->restore_stmt_binlog_format(save_binlog_format);
  if (thd->commit_whole_transaction_and_close_tables())
    rc= 1;
  new_trans.restore_old_transaction();

  DBUG_RETURN(rc);
}


/**
  @brief
  Delete statistics on an index of the specified table

  @param thd         The thread handle
  @param tab         The table the index belongs to
  @param key_info    The descriptor of the index whose statistics is to be deleted
  @param ext_prefixes_only  Delete statistics only on the index prefixes
                     extended by the components of the primary key

  @details
  The function delete statistics on the index  specified by 'key_info'
  defined on the table 'tab' from the statistical table index_stats.

  @retval 0  If all deletions are successful or we couldn't open statistics table
  @retval 1  Otherwise

  @note
  The function is called when dropping an index, or dropping/changing the
   definition of a column used in the definition of the index. 
*/

int delete_statistics_for_index(THD *thd, TABLE *tab, KEY *key_info,
                                bool ext_prefixes_only)
{
  int err;
  enum_binlog_format save_binlog_format;
  TABLE *stat_table;
  TABLE_LIST tables;
  int rc= 0;
  DBUG_ENTER("delete_statistics_for_index");
   
  start_new_trans new_trans(thd);

  if (open_stat_table_for_ddl(thd, &tables, &stat_table_name[2]))
    DBUG_RETURN(0);

  save_binlog_format= thd->set_current_stmt_binlog_format_stmt();

  stat_table= tables.table;
  Index_stat index_stat(stat_table, tab);
  if (!ext_prefixes_only)
  {
    index_stat.set_index_prefix_key_fields(key_info);
    while (index_stat.find_next_stat_for_prefix(3))
    {
      err= index_stat.delete_stat();
      if (err && !rc)
        rc= 1;
    }
  }
  else
  {
    for (uint i= key_info->user_defined_key_parts; i < key_info->ext_key_parts; i++)
    {
      index_stat.set_key_fields(key_info, i+1);
      if (index_stat.find_next_stat_for_prefix(4))
      {
        err= index_stat.delete_stat();
        if (err && !rc)
          rc= 1;
      }
    }
  }

  err= del_global_index_stat(thd, tab, key_info);
  if (err && !rc)
    rc= 1;

  thd->restore_stmt_binlog_format(save_binlog_format);
  if (thd->commit_whole_transaction_and_close_tables())
    rc= 1;
  new_trans.restore_old_transaction();

  DBUG_RETURN(rc);
}


/**
  @brief
  Rename a table in all statistical tables

  @param
  thd         The thread handle
  @param
  db          The name of the database the table belongs to
  @param
  tab         The name of the table to be renamed in statistical tables
  @param
  new_tab     The new name of the table

  @details
  The function replaces the name of the table 'tab' from the database 'db' 
  for 'new_tab' in all all statistical tables: table_stats, column_stats,
  index_stats.

  @retval
  0         If all updates of the table name are successful
  @retval
  1         Otherwise

  @note
  The function is called when executing any statement that renames a table
*/

int rename_table_in_stat_tables(THD *thd, const LEX_CSTRING *db,
                                const LEX_CSTRING *tab,
                                const LEX_CSTRING *new_db,
                                const LEX_CSTRING *new_tab)
{
  int err;
  enum_binlog_format save_binlog_format;
  TABLE *stat_table;
  TABLE_LIST tables[STATISTICS_TABLES];
  int rc= 0;
  DBUG_ENTER("rename_table_in_stat_tables");
   
  start_new_trans new_trans(thd);

  if (open_stat_tables(thd, tables, TRUE))
    DBUG_RETURN(0); // not an error

  save_binlog_format= thd->set_current_stmt_binlog_format_stmt();

  /* Rename table in the statistical table index_stats */
  stat_table= tables[INDEX_STAT].table;
  Index_stat index_stat(stat_table, db, tab);
  index_stat.set_full_table_name();

  Stat_table_write_iter index_iter(&index_stat);
  if (index_iter.init(2))
    rc= 1;
  while (!index_iter.get_next_row())
  {
    err= index_stat.update_table_name_key_parts(new_db, new_tab);
    if (err & !rc)
      rc= 1;
    index_stat.set_full_table_name();
  }
  index_iter.cleanup();

  /* Rename table in the statistical table column_stats */
  stat_table= tables[COLUMN_STAT].table;
  Column_stat column_stat(stat_table, db, tab);
  column_stat.set_full_table_name();
  Stat_table_write_iter column_iter(&column_stat);
  if (column_iter.init(2))
    rc= 1;
  while (!column_iter.get_next_row())
  {
    err= column_stat.update_table_name_key_parts(new_db, new_tab);
    if (err & !rc)
      rc= 1;
    column_stat.set_full_table_name();
  }
  column_iter.cleanup();
   
  /* Rename table in the statistical table table_stats */
  stat_table= tables[TABLE_STAT].table;
  Table_stat table_stat(stat_table, db, tab);
  table_stat.set_key_fields();
  if (table_stat.find_stat())
  {
    err= table_stat.update_table_name_key_parts(new_db, new_tab);
    if (err & !rc)
      rc= 1;
  }

  thd->restore_stmt_binlog_format(save_binlog_format);
  if (thd->commit_whole_transaction_and_close_tables())
    rc= 1;
  new_trans.restore_old_transaction();

  DBUG_RETURN(rc);
}


/**
  Rename a column in the statistical table column_stats

  @param thd         The thread handle
  @param tab         The table the column belongs to
  @param col         The column to be renamed
  @param new_name    The new column name

  @details
  The function replaces the name of the column 'col' belonging to the table 
  'tab' for 'new_name' in the statistical table column_stats.

  @retval 0  If all updates of the table name are successful
  @retval 1  Otherwise

  @note
  The function is called when executing any statement that renames a column,
  but does not change the column definition.
*/

int rename_column_in_stat_tables(THD *thd, TABLE *tab, Field *col,
                                 const char *new_name)
{
  int err;
  enum_binlog_format save_binlog_format;
  TABLE *stat_table;
  TABLE_LIST tables;
  int rc= 0;
  DBUG_ENTER("rename_column_in_stat_tables");
  
  if (tab->s->tmp_table != NO_TMP_TABLE)
    DBUG_RETURN(0);

  start_new_trans new_trans(thd);

  if (open_stat_table_for_ddl(thd, &tables, &stat_table_name[1]))
    DBUG_RETURN(rc);

  save_binlog_format= thd->set_current_stmt_binlog_format_stmt();

  /* Rename column in the statistical table table_stat */
  stat_table= tables.table;
  Column_stat column_stat(stat_table, tab);
  column_stat.set_key_fields(col);
  if (column_stat.find_stat())
  { 
    err= column_stat.update_column_key_part(new_name);
    if (err & !rc)
      rc= 1;
  }

  thd->restore_stmt_binlog_format(save_binlog_format);
  if (thd->commit_whole_transaction_and_close_tables())
    rc= 1;
  new_trans.restore_old_transaction();

  DBUG_RETURN(rc);
}


/**
  @brief
  Set statistics for a table that will be used by the optimizer 

  @param
  thd         The thread handle
  @param
  table       The table to set statistics for 

  @details
  Depending on the value of thd->variables.use_stat_tables 
  the function performs the settings for the table that will control
  from where the statistical data used by the optimizer will be taken.
*/

void set_statistics_for_table(THD *thd, TABLE *table)
{
  TABLE_STATISTICS_CB *stats_cb= &table->s->stats_cb;
  Table_statistics *read_stats= stats_cb->table_stats;
  table->used_stat_records= 
    (!check_eits_preferred(thd) ||
     !table->stats_is_read || read_stats->cardinality_is_null) ?
    table->file->stats.records : read_stats->cardinality;

  /*
    For partitioned table, EITS statistics is based on data from all partitions.

    On the other hand, Partition Pruning figures which partitions will be
    accessed and then computes the estimate of rows in used_partitions.

    Use the estimate from Partition Pruning as it is typically more precise.
    Ideally, EITS should provide per-partition statistics but this is not
    implemented currently.
  */
#ifdef WITH_PARTITION_STORAGE_ENGINE
    if (table->part_info)
      table->used_stat_records= table->file->stats.records;
#endif

  KEY *key_info, *key_info_end;
  for (key_info= table->key_info, key_info_end= key_info+table->s->keys;
       key_info < key_info_end; key_info++)
  {
    key_info->is_statistics_from_stat_tables=
      (check_eits_preferred(thd) &&
       table->stats_is_read &&
       key_info->read_stats->avg_frequency_is_inited() &&
       key_info->read_stats->get_avg_frequency(0) > 0.5);
  }
}


/**
  @brief
  Get the average frequency for a column 

  @param
  field       The column whose average frequency is required

  @retval
  The required average frequency
*/

double get_column_avg_frequency(Field * field)
{
  double res;
  TABLE *table= field->table;

  /* 
    Statistics is shared by table instances and  is accessed through
    the table share. If table->s->field is not set for 'table', then
    no column statistics is available for the table .
  */
  if (!table->s->field)
  {
    res= (double)table->stat_records();
    return res;
  }
 
  Column_statistics *col_stats= field->read_stats;

  if (!col_stats)
    res= (double)table->stat_records();
  else
    res= col_stats->get_avg_frequency();
  return res;
} 


/**
  @brief
  Estimate the number of rows in a column range using data from stat tables 

  @param
  field       The column whose range cardinality is to be estimated
  @param
  min_endp    The left end of the range whose cardinality is required 
  @param
  max_endp    The right end of the range whose cardinality is required 
  @param
  range_flag  The range flags

  @details
  The function gets an estimate of the number of rows in a column range
  using the statistical data from the table column_stats.

  @retval
  - The required estimate of the rows in the column range
  - If there is some kind of error, this function should return DBL_MAX (and
    not HA_POS_ERROR as that is an integer constant).

*/

double get_column_range_cardinality(Field *field,
                                    key_range *min_endp,
                                    key_range *max_endp,
                                    uint range_flag)
{
  double res;
  TABLE *table= field->table;
  Column_statistics *col_stats= field->read_stats;
  double tab_records= (double)table->stat_records();

  if (!col_stats)
    return tab_records;
  /*
    Use statistics for a table only when we have actually read
    the statistics from the stat tables. For example due to
    chances of getting a deadlock we disable reading statistics for
    a table.
  */

  if (!table->stats_is_read)
    return tab_records;

  THD *thd= table->in_use;
  double col_nulls= tab_records * col_stats->get_nulls_ratio();

  double col_non_nulls= tab_records - col_nulls;

  bool nulls_incl= field->null_ptr && min_endp && min_endp->key[0] &&
                   !(range_flag & NEAR_MIN);

  if (col_non_nulls < 1)
  {
    if (nulls_incl)
      res= col_nulls;
    else
      res= 0;
  }
  else if (min_endp && max_endp && min_endp->length == max_endp->length &&
           !memcmp(min_endp->key, max_endp->key, min_endp->length))
  { 
    if (nulls_incl)
    {
      /* This is null single point range */
      res= col_nulls;
    }
    else
    {
      double avg_frequency= col_stats->get_avg_frequency();
      res= avg_frequency;   
      if (avg_frequency > 1.0 + 0.000001 && 
          col_stats->min_max_values_are_provided())
      {
        Histogram_base *hist = col_stats->histogram;
        if (hist && hist->is_usable(thd))
        {
          res= col_non_nulls * 
	       hist->point_selectivity(field, min_endp,
                                       avg_frequency / col_non_nulls);
        }
      }
      else if (avg_frequency == 0.0)
      {
        /* This actually means there is no statistics data */
        res= tab_records;
      }
    }
  }  
  else 
  {
    if (col_stats->min_max_values_are_provided())
    {
      Histogram_base *hist= col_stats->histogram;
      double avg_frequency= col_stats->get_avg_frequency();
      double sel;
      if (hist && hist->is_usable(thd))
      {
        sel= hist->range_selectivity(field, min_endp, max_endp,
                                     avg_frequency / col_non_nulls);
        res= col_non_nulls * sel;
      }
      else
      {
        double min_mp_pos, max_mp_pos;
        if (min_endp && !(field->null_ptr && min_endp->key[0]))
        {
          store_key_image_to_rec(field, (uchar *) min_endp->key,
                                 field->key_length());
          min_mp_pos=
              field->pos_in_interval(col_stats->min_value, col_stats->max_value);
        }
        else
          min_mp_pos= 0.0;
        if (max_endp)
        {
          store_key_image_to_rec(field, (uchar *) max_endp->key,
                                 field->key_length());
          max_mp_pos=
              field->pos_in_interval(col_stats->min_value, col_stats->max_value);
        }
        else
          max_mp_pos= 1.0;

        sel = (max_mp_pos - min_mp_pos);
        res= col_non_nulls * sel;
        set_if_bigger(res, avg_frequency);
      }
    }
    else
      res= col_non_nulls;
    if (nulls_incl)
      res+= col_nulls;
  }
  return res;
}

/*
  Estimate selectivity of "col=const" using a histogram
  
  @param field    the field to estimate its selectivity.

  @param endpoint The constant

  @param avg_sel  Average selectivity of condition "col=const" in this table.
                  It is calcuated as (#non_null_values / #distinct_values).
  
  @return
     Expected condition selectivity (a number between 0 and 1)

  @notes 
     [re_zero_length_buckets] If a bucket with zero value-length is in the
     middle of the histogram, we will not have min==max. Example: suppose, 
     pos_value=0x12, and the histogram is:

           #n  #n+1 #n+2                 
      ... 0x10 0x12 0x12 0x14 ...
                      |
                      +------------- bucket with zero value-length
    
      Here, we will get min=#n+1, max=#n+2, and use the multi-bucket formula.
     
      The problem happens at the histogram ends. if pos_value=0, and the
      histogram is:

      0x00 0x10 ...

      then min=0, max=0. This means pos_value is contained within bucket #0,
      but on the other hand, histogram data says that the bucket has only one
      value.
*/

double Histogram_binary::point_selectivity(Field *field, key_range *endpoint,
                                           double avg_sel)
{
  double sel;
  Column_statistics *col_stats= field->read_stats;
  store_key_image_to_rec(field, (uchar *) endpoint->key,
                         field->key_length());
  double pos= field->pos_in_interval(col_stats->min_value,
                                     col_stats->max_value);
  /* Find the bucket that contains the value 'pos'. */
  uint min= find_bucket(pos, TRUE);
  uint pos_value= (uint) (pos * prec_factor());

  /* Find how many buckets this value occupies */
  uint max= min;
  while (max + 1 < get_width() && get_value(max + 1) == pos_value)
    max++;
  
  /*
    A special case: we're looking at a single bucket, and that bucket has
    zero value-length. Use the multi-bucket formula (attempt to use
    single-bucket formula will cause divison by zero).

    For more details see [re_zero_length_buckets] above.
  */
  if (max == min && get_value(max) == ((max==0)? 0 : get_value(max-1)))
    max++;

  if (max > min)
  {
    /*
      The value occupies multiple buckets. Use start_bucket ... end_bucket as
      selectivity.
    */
    double bucket_sel= 1.0/(get_width() + 1);  
    sel= bucket_sel * (max - min + 1);
  }
  else
  {
    /* 
      The value 'pos' fits within one single histogram bucket.

      Histogram_binary buckets have the same numbers of rows, but they cover
      different ranges of values.

      We assume that values are uniformly distributed across the [0..1] value
      range.
    */

    /* 
      If all buckets covered value ranges of the same size, the width of
      value range would be:
    */
    double avg_bucket_width= 1.0 / (get_width() + 1);
    
    /*
      Let's see what is the width of value range that our bucket is covering.
        (min==max currently. they are kept in the formula just in case we 
         will want to extend it to handle multi-bucket case)
    */
    double inv_prec_factor= (double) 1.0 / prec_factor(); 
    double current_bucket_width= 
        (max + 1 == get_width() ?  1.0 : (get_value(max) * inv_prec_factor)) -
        (min == 0 ?  0.0 : (get_value(min-1) * inv_prec_factor));

    DBUG_ASSERT(current_bucket_width); /* We shouldn't get a one zero-width bucket */

    /*
      So:
      - each bucket has the same #rows 
      - values are unformly distributed across the [min_value,max_value] domain.

      If a bucket has value range that's N times bigger then average, than
      each value will have to have N times fewer rows than average.
    */
    sel= avg_sel * avg_bucket_width / current_bucket_width;

    /*
      (Q: if we just follow this proportion we may end up in a situation
      where number of different values we expect to find in this bucket
      exceeds the number of rows that this histogram has in a bucket. Are 
      we ok with this or we would want to have certain caps?)
    */
  }
  return sel;
}


double Histogram_binary::range_selectivity(Field *field,
                                           key_range *min_endp,
                                           key_range *max_endp,
                                           double avg_sel)
{
  double sel, min_mp_pos, max_mp_pos;
  Column_statistics *col_stats= field->read_stats;

  if (min_endp && !(field->null_ptr && min_endp->key[0]))
  {
    store_key_image_to_rec(field, (uchar *) min_endp->key,
                           field->key_length());
    min_mp_pos=
        field->pos_in_interval(col_stats->min_value, col_stats->max_value);
  }
  else
    min_mp_pos= 0.0;
  if (max_endp)
  {
    store_key_image_to_rec(field, (uchar *) max_endp->key,
                           field->key_length());
    max_mp_pos=
        field->pos_in_interval(col_stats->min_value, col_stats->max_value);
  }
  else
    max_mp_pos= 1.0;

  double bucket_sel= 1.0 / (get_width() + 1);
  uint min= find_bucket(min_mp_pos, TRUE);
  uint max= find_bucket(max_mp_pos, FALSE);
  sel= bucket_sel * (max - min + 1);

  set_if_bigger(sel, avg_sel);
  return sel;
}

/*
  Check whether the table is one of the persistent statistical tables.
*/
bool is_stat_table(const LEX_CSTRING *db, LEX_CSTRING *table)
{
  DBUG_ASSERT(db->str && table->str);

  if (!my_strcasecmp(table_alias_charset, db->str, MYSQL_SCHEMA_NAME.str))
  {
    for (uint i= 0; i < STATISTICS_TABLES; i ++)
    {
      if (!my_strcasecmp(table_alias_charset, table->str, stat_table_name[i].str))
        return true;
    }
  }
  return false;
}

/*
  Check wheter we can use EITS statistics for a field or not

  TRUE : Use EITS for the columns
  FALSE: Otherwise
*/

bool is_eits_usable(Field *field)
{
  Column_statistics* col_stats= field->read_stats;
  
  // check if column_statistics was allocated for this field
  if (!col_stats)
    return false;

  DBUG_ASSERT(field->table->stats_is_read);

  /*
    (1): checks if we have EITS statistics for a particular column
    (2): Don't use EITS for GEOMETRY columns
    (3): Disabling reading EITS statistics for columns involved in the
         partition list of a table. We assume the selecticivity for
         such columns would be handled during partition pruning.
  */

  return !col_stats->no_stat_values_provided() &&        //(1)
    field->type() != MYSQL_TYPE_GEOMETRY &&              //(2)
#ifdef WITH_PARTITION_STORAGE_ENGINE
    (!field->table->part_info ||
     !field->table->part_info->field_in_partition_expr(field)) &&     //(3)
#endif
    true;
}
