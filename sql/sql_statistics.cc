/* Copyright (C) 2009 MySQL AB

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

/**
  @file

  @brief
  functions to update persitent statistical tables and to read from them

  @defgroup Query_Optimizer  Query Optimizer
  @{
*/

#include <my_global.h>
#include "sql_base.h"
#include "key.h"
#include "sql_statistics.h"
#include "opt_range.h"
#include "my_atomic.h"

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
   
/* Currently there are only 3 persistent statistical tables */
static const uint STATISTICS_TABLES= 3;

/* 
  The names of the statistical tables in this array must correspond the
  definitions of the tables in the file ../scripts/mysql_system_tables.sql
*/
static const LEX_STRING stat_table_name[STATISTICS_TABLES]=
{
  { C_STRING_WITH_LEN("table_stats") },
  { C_STRING_WITH_LEN("column_stats") },
  { C_STRING_WITH_LEN("index_stats") }
};

/* Name of database to which the statistical tables belong */
static const LEX_STRING stat_tables_db_name= { C_STRING_WITH_LEN("mysql") };


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
    tables[i].db= stat_tables_db_name.str;
    tables[i].db_length= stat_tables_db_name.length;
    tables[i].alias= tables[i].table_name= stat_table_name[i].str;
    tables[i].table_name_length= stat_table_name[i].length;
    tables[i].lock_type= for_write ? TL_WRITE : TL_READ;
    if (i < STATISTICS_TABLES - 1)
    tables[i].next_global= tables[i].next_local=
      tables[i].next_name_resolution_table= &tables[i+1];
    if (i != 0)
      tables[i].prev_global= &tables[i-1].next_global;
  }
}


/**
  @details
  The function builds a TABLE_LIST containing only one element 'tbl' for
  the statistical table called 'stat_tab_name'. 
  The lock type of the element is set to TL_READ if for_write = FALSE,
  otherwise it is set to TL_WRITE.
*/

static
inline void init_table_list_for_single_stat_table(TABLE_LIST *tbl,
                                                  const LEX_STRING *stat_tab_name, 
                                                  bool for_write)
{
  memset((char *) tbl, 0, sizeof(TABLE_LIST));

  tbl->db= stat_tables_db_name.str;
  tbl->db_length= stat_tables_db_name.length;
  tbl->alias= tbl->table_name= stat_tab_name->str;
  tbl->table_name_length= stat_tab_name->length;
  tbl->lock_type= for_write ? TL_WRITE : TL_READ;
}


/**
  @brief
  Open all statistical tables and lock them
*/

static
inline int open_stat_tables(THD *thd, TABLE_LIST *tables,
                            Open_tables_backup *backup,
                            bool for_write)
{
  init_table_list_for_stat_tables(tables, for_write);
  init_mdl_requests(tables);
  return open_system_tables_for_read(thd, tables, backup);
}


/**
  @brief
  Open a statistical table and lock it
*/
static
inline int open_single_stat_table(THD *thd, TABLE_LIST *table,
                                  const LEX_STRING *stat_tab_name,
                                  Open_tables_backup *backup,
                                  bool for_write)
{
  init_table_list_for_single_stat_table(table, stat_tab_name, for_write);
  init_mdl_requests(table);
  return open_system_tables_for_read(thd, table, backup);
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
  inline bool add(ha_rows rowno);
  inline void finish(ha_rows rows); 
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
  LEX_STRING *db_name;      /* Name of the database containing 'table' */ 
  LEX_STRING *table_name;   /* Name of the table 'table' */

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
  
  Stat_table(TABLE *stat, LEX_STRING *db, LEX_STRING *tab)
    :stat_table(stat), table_share(NULL)
  {
    common_init_stat_table();
    db_name= db;
    table_name= tab;
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

 virtual void change_full_table_name(LEX_STRING *db, LEX_STRING *tab)= 0;

 
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
      store_record_for_update();
      store_stat_fields();
      return update_record();
    }
    else
    {
      int err;
      store_stat_fields();
      if ((err= stat_file->ha_write_row(record[0])))
	return TRUE;
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

  bool update_table_name_key_parts(LEX_STRING *db, LEX_STRING *tab)
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

  void change_full_table_name(LEX_STRING *db, LEX_STRING *tab)
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

  Table_stat(TABLE *stat, LEX_STRING *db, LEX_STRING *tab) 
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
      stat_field->store(table->collected_stats->cardinality);
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

  void change_full_table_name(LEX_STRING *db, LEX_STRING *tab)
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

  Column_stat(TABLE *stat, LEX_STRING *db, LEX_STRING *tab) 
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
    const char *column_name= col->field_name;
    column_name_field->store(column_name, strlen(column_name),
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
    char buff[MAX_FIELD_WIDTH];
    String val(buff, sizeof(buff), &my_charset_bin);

    for (uint i= COLUMN_STAT_MIN_VALUE; i <= COLUMN_STAT_HISTOGRAM; i++)
    {  
      Field *stat_field= stat_table->field[i];
      if (table_field->collected_stats->is_null(i))
        stat_field->set_null();
      else
      {
        stat_field->set_notnull();
        switch (i) {
        case COLUMN_STAT_MIN_VALUE:
          if (table_field->type() == MYSQL_TYPE_BIT)
            stat_field->store(table_field->collected_stats->min_value->val_int());
          else
          {
            table_field->collected_stats->min_value->val_str(&val);
            stat_field->store(val.ptr(), val.length(), &my_charset_bin);
          }
          break;
        case COLUMN_STAT_MAX_VALUE:
          if (table_field->type() == MYSQL_TYPE_BIT)
            stat_field->store(table_field->collected_stats->max_value->val_int());
          else
          {
            table_field->collected_stats->max_value->val_str(&val);
            stat_field->store(val.ptr(), val.length(), &my_charset_bin);
          }
          break;
        case COLUMN_STAT_NULLS_RATIO:
          stat_field->store(table_field->collected_stats->get_nulls_ratio());
          break;
        case COLUMN_STAT_AVG_LENGTH:
          stat_field->store(table_field->collected_stats->get_avg_length());
          break;
        case COLUMN_STAT_AVG_FREQUENCY:
          stat_field->store(table_field->collected_stats->get_avg_frequency());
          break; 
        case COLUMN_STAT_HIST_SIZE:
          stat_field->store(table_field->collected_stats->histogram.get_size());
          break;
        case COLUMN_STAT_HIST_TYPE:
          stat_field->store(table_field->collected_stats->histogram.get_type() +
                            1);
          break;
        case COLUMN_STAT_HISTOGRAM:
          const char * col_histogram=
          (const char *) (table_field->collected_stats->histogram.get_values());
	  stat_field->store(col_histogram,
                            table_field->collected_stats->histogram.get_size(),
                            &my_charset_bin);
          break;           
        }
      }
    }
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

    if (table_field->read_stats->min_value)
      table_field->read_stats->min_value->set_null();
    if (table_field->read_stats->max_value)
      table_field->read_stats->max_value->set_null();

    if (find_stat())
    {
      char buff[MAX_FIELD_WIDTH];
      String val(buff, sizeof(buff), &my_charset_bin);

      for (uint i= COLUMN_STAT_MIN_VALUE; i <= COLUMN_STAT_HIST_TYPE; i++)
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
            stat_field->val_str(&val);
            table_field->read_stats->min_value->store(val.ptr(), val.length(),
                                                      &my_charset_bin);
            break;
          case COLUMN_STAT_MAX_VALUE:
            stat_field->val_str(&val);
            table_field->read_stats->max_value->store(val.ptr(), val.length(),
                                                      &my_charset_bin);
            break;
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
            table_field->read_stats->histogram.set_size(stat_field->val_int());
            break;            
          case COLUMN_STAT_HIST_TYPE:
            Histogram_type hist_type= (Histogram_type) (stat_field->val_int() -
                                                        1);
            table_field->read_stats->histogram.set_type(hist_type);
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

  void get_histogram_value()
  {
    if (find_stat())
    {
      char buff[MAX_FIELD_WIDTH];
      String val(buff, sizeof(buff), &my_charset_bin);
      uint fldno= COLUMN_STAT_HISTOGRAM;
      Field *stat_field= stat_table->field[fldno];
      table_field->read_stats->set_not_null(fldno);
      stat_field->val_str(&val);
      memcpy(table_field->read_stats->histogram.get_values(),
             val.ptr(), table_field->read_stats->histogram.get_size());
    }
  }

};


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

  void change_full_table_name(LEX_STRING *db, LEX_STRING *tab)
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

  Index_stat(TABLE *stat, LEX_STRING *db, LEX_STRING *tab) 
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
    char *index_name= index_info->name;
    index_name_field->store(index_name, strlen(index_name),
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
    if (!(rowid_buf= (uchar*)my_malloc(rowid_size, MYF(0))))
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
    cleanup();
  }
};

/*
  Histogram_builder is a helper class that is used to build histograms
  for columns
*/

class Histogram_builder
{
  Field *column;           /* table field for which the histogram is built */
  uint col_length;         /* size of this field                           */
  ha_rows records;         /* number of records the histogram is built for */
  Field *min_value;        /* pointer to the minimal value for the field   */
  Field *max_value;        /* pointer to the maximal value for the field   */
  Histogram *histogram;    /* the histogram location                       */
  uint hist_width;         /* the number of points in the histogram        */
  double bucket_capacity;  /* number of rows in a bucket of the histogram  */ 
  uint curr_bucket;        /* number of the current bucket to be built     */
  ulonglong count;         /* number of values retrieved                   */
  ulonglong count_distinct;    /* number of distinct values retrieved      */

public: 
  Histogram_builder(Field *col, uint col_len, ha_rows rows)
    : column(col), col_length(col_len), records(rows)
  {
    Column_statistics *col_stats= col->collected_stats;
    min_value= col_stats->min_value;
    max_value= col_stats->max_value;
    histogram= &col_stats->histogram;
    hist_width= histogram->get_width();
    bucket_capacity= (double) records / (hist_width + 1);
    curr_bucket= 0;
    count= 0;
    count_distinct= 0;    
  }

  ulonglong get_count_distinct() { return count_distinct; }

  int next(void *elem, element_count elem_cnt)
  {
    count_distinct++;
    count+= elem_cnt;
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
};


C_MODE_START

int histogram_build_walk(void *elem, element_count elem_cnt, void *arg)
{
  Histogram_builder *hist_builder= (Histogram_builder *) arg;
  return hist_builder->next(elem, elem_cnt);
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

  Count_distinct_field(Field *field, uint max_heap_table_size)
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
    return tree->unique_add(table_field->ptr);
  }
  
  /*
    @brief
    Calculate the number of elements accumulated in the container of 'tree'
  */
  ulonglong get_value()
  {
    ulonglong count;
    if (tree->elements == 0)
      return (ulonglong) tree->elements_in_tree();
    count= 0;  
    tree->walk(table_field->table, count_distinct_walk, (void*) &count);
    return count;
  }

  /*
    @brief
    Build the histogram for the elements accumulated in the container of 'tree'
  */
  ulonglong get_value_with_histogram(ha_rows rows)
  {
    Histogram_builder hist_builder(table_field, tree_key_length, rows);
    tree->walk(table_field->table,  histogram_build_walk, (void *) &hist_builder);
    return hist_builder.get_count_distinct();
  }

  /*
    @brief
    Get the size of the histogram in bytes built for table_field
  */
  uint get_hist_size()
  {
    return table_field->collected_stats->histogram.get_size();
  }

  /*
    @brief
    Get the pointer to the histogram built for table_field
  */
  uchar *get_histogram()
  {
    return table_field->collected_stats->histogram.get_values();
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

  Count_distinct_field_bit(Field *field, uint max_heap_table_size)
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

  Index_prefix_calc(TABLE *table, KEY *key_info)
    : index_table(table), index_info(key_info)
  {
    uint i;
    Prefix_calc_state *state;
    uint key_parts= table->actual_n_key_parts(key_info);
    empty= TRUE;
    prefixes= 0;
    LINT_INIT_STRUCT(calc_state);

    is_single_comp_pk= FALSE;
    uint pk= table->s->primary_key;
    if ((uint) (table->key_info - key_info) == pk &&
        table->key_info[pk].user_defined_key_parts == 1)
    {
      prefixes= 1;
      is_single_comp_pk= TRUE;
      return;
    }
        
    if ((calc_state=
         (Prefix_calc_state *) sql_alloc(sizeof(Prefix_calc_state)*key_parts)))
    {
      uint keyno= key_info-table->key_info;
      for (i= 0, state= calc_state; i < key_parts; i++, state++)
      {
        /* 
          Do not consider prefixes containing a component that is only part
          of the field. This limitation is set to avoid fetching data when
          calculating the values of 'avg_frequency' for prefixes.
	*/   
        if (!key_info->key_part[i].field->part_of_key.is_set(keyno))
          break;

        if (!(state->last_prefix=
              new Cached_item_field(key_info->key_part[i].field)))
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
        if (!(fld= table_field->clone(&table->mem_root, table, diff, TRUE)))
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
        if (!(fld= table_field->clone(&stats_cb->mem_root, diff)))
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
  uint fields;

  DBUG_ENTER("alloc_statistics_for_table");


  Table_statistics *table_stats= 
    (Table_statistics *) alloc_root(&table->mem_root,
                                    sizeof(Table_statistics));

  fields= table->s->fields ; 
  Column_statistics_collected *column_stats=
    (Column_statistics_collected *) alloc_root(&table->mem_root,
                                    sizeof(Column_statistics_collected) *
				    (fields+1));

  uint keys= table->s->keys;
  Index_statistics *index_stats=
    (Index_statistics *) alloc_root(&table->mem_root,
                                    sizeof(Index_statistics) * keys);

  uint key_parts= table->s->ext_key_parts;
  ulong *idx_avg_frequency= (ulong*) alloc_root(&table->mem_root,
                                                sizeof(ulong) * key_parts);

  uint columns= 0;
  for (field_ptr= table->field; *field_ptr; field_ptr++)
  {
    if (bitmap_is_set(table->read_set, (*field_ptr)->field_index))
      columns++;
  }
  uint hist_size= thd->variables.histogram_size;
  Histogram_type hist_type= (Histogram_type) (thd->variables.histogram_type);
  uchar *histogram= NULL;
  if (hist_size > 0)
    histogram= (uchar *) alloc_root(&table->mem_root, hist_size * columns);

  if (!table_stats || !column_stats || !index_stats || !idx_avg_frequency ||
      (hist_size && !histogram))
    DBUG_RETURN(1);

  table->collected_stats= table_stats;
  table_stats->column_stats= column_stats;
  table_stats->index_stats= index_stats;
  table_stats->idx_avg_frequency= idx_avg_frequency;
  table_stats->histograms= histogram;
  
  memset(column_stats, 0, sizeof(Column_statistics) * (fields+1));

  for (field_ptr= table->field; *field_ptr; field_ptr++, column_stats++)
  {
    (*field_ptr)->collected_stats= column_stats;
    (*field_ptr)->collected_stats->max_value= NULL;
    (*field_ptr)->collected_stats->min_value= NULL;
    if (bitmap_is_set(table->read_set, (*field_ptr)->field_index))
    {
      column_stats->histogram.set_size(hist_size);
      column_stats->histogram.set_type(hist_type);
      column_stats->histogram.set_values(histogram);
      histogram+= hist_size;
    }
  }

  memset(idx_avg_frequency, 0, sizeof(ulong) * key_parts);

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


/**
  @brief
  Check whether any persistent statistics for the processed command is needed

  @param
  thd         The thread handle

  @details
  The function checks whether any persitent statistics for the processed
  command is needed to be read.

  @retval
  TRUE        statistics is needed to be read 
  @retval
  FALSE       Otherwise
*/

static
inline bool statistics_for_command_is_needed(THD *thd)
{
  if (thd->bootstrap || thd->variables.use_stat_tables == NEVER)
    return FALSE;
  
  switch(thd->lex->sql_command) {
  case SQLCOM_SELECT:
  case SQLCOM_INSERT:
  case SQLCOM_INSERT_SELECT:
  case SQLCOM_UPDATE:
  case SQLCOM_UPDATE_MULTI:
  case SQLCOM_DELETE:
  case SQLCOM_DELETE_MULTI:
  case SQLCOM_REPLACE:
  case SQLCOM_REPLACE_SELECT:
    break;
  default: 
    return FALSE;
  }

  return TRUE;
} 


/**
  @brief 
  Allocate memory for the statistical data used by a table share

  @param
  thd         Thread handler
  @param
  table_share Table share for which the memory for statistical data is allocated
  @param
  is_safe     TRUE <-> at any time only one thread can perform the function

  @note
  The function allocates the memory for the statistical data on a table in the
  table's share memory with the intention to read the statistics there from
  the system persistent statistical tables mysql.table_stat, mysql.column_stats,
  mysql.index_stats. The memory is allocated for the statistics on the table,
  on the tables's columns, and on the table's indexes. The memory is allocated
  in the table_share's mem_root.
  If the parameter is_safe is TRUE then it is guaranteed that at any given time
  only one thread is executed the code of the function.

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

  @note
  Currently the function always is called with the parameter is_safe set
  to FALSE. 
*/      

int alloc_statistics_for_table_share(THD* thd, TABLE_SHARE *table_share, 
                                     bool is_safe)
{
  
  Field **field_ptr;
  KEY *key_info, *end;
  TABLE_STATISTICS_CB *stats_cb= &table_share->stats_cb;

  DBUG_ENTER("alloc_statistics_for_table_share");

  DEBUG_SYNC(thd, "statistics_mem_alloc_start1");
  DEBUG_SYNC(thd, "statistics_mem_alloc_start2");

  if (!statistics_for_command_is_needed(thd))
    DBUG_RETURN(1);

  if (!is_safe)
    mysql_mutex_lock(&table_share->LOCK_share);

  if (stats_cb->stats_can_be_read)
  {
    if (!is_safe)
      mysql_mutex_unlock(&table_share->LOCK_share);
    DBUG_RETURN(0);
  }

  Table_statistics *table_stats= stats_cb->table_stats;
  if (!table_stats)
  {
    table_stats=  (Table_statistics *) alloc_root(&stats_cb->mem_root,
                                                  sizeof(Table_statistics));
    if (!table_stats)
    {
      if (!is_safe)
        mysql_mutex_unlock(&table_share->LOCK_share);
      DBUG_RETURN(1);
    }
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
  ulong *idx_avg_frequency=  table_stats->idx_avg_frequency;
  if (!idx_avg_frequency)
  {
    idx_avg_frequency= (ulong*) alloc_root(&stats_cb->mem_root,
                                           sizeof(ulong) * key_parts);
    if (idx_avg_frequency)
    {
      memset(idx_avg_frequency, 0, sizeof(ulong) * key_parts);
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

  if (column_stats && index_stats && idx_avg_frequency)
    stats_cb->stats_can_be_read= TRUE;

  if (!is_safe)
    mysql_mutex_unlock(&table_share->LOCK_share);

  DBUG_RETURN(0);
}


/**
  @brief 
  Allocate memory for the histogram used by a table share

  @param
  thd         Thread handler
  @param
  table_share Table share for which the memory for histogram data is allocated
  @param
  is_safe     TRUE <-> at any time only one thread can perform the function

  @note
  The function allocates the memory for the histogram built for a table in the
  table's share memory with the intention to read the data there from the
  system persistent statistical table mysql.column_stats,
  The memory is allocated in the table_share's mem_root.
  If the parameter is_safe is TRUE then it is guaranteed that at any given time
  only one thread is executed the code of the function.

  @retval
  0     If the memory for all statistical data has been successfully allocated  
  @retval
  1     Otherwise

  @note
  Currently the function always is called with the parameter is_safe set
  to FALSE. 
*/      

static
int alloc_histograms_for_table_share(THD* thd, TABLE_SHARE *table_share, 
                                     bool is_safe)
{
  TABLE_STATISTICS_CB *stats_cb= &table_share->stats_cb;

  DBUG_ENTER("alloc_histograms_for_table_share");

  if (!is_safe)
    mysql_mutex_lock(&table_share->LOCK_share);

  if (stats_cb->histograms_can_be_read)
  {
    if (!is_safe)
      mysql_mutex_unlock(&table_share->LOCK_share);
    DBUG_RETURN(0);
  }

  Table_statistics *table_stats= stats_cb->table_stats;
  ulong total_hist_size= table_stats->total_hist_size;

  if (total_hist_size && !table_stats->histograms)
  {
    uchar *histograms= (uchar *) alloc_root(&stats_cb->mem_root,
                                            total_hist_size);
    if (!histograms)
    {
      if (!is_safe)
        mysql_mutex_unlock(&table_share->LOCK_share);
      DBUG_RETURN(1);
    }
    memset(histograms, 0, total_hist_size);
    table_stats->histograms= histograms;
    stats_cb->histograms_can_be_read= TRUE;
  }

  if (!is_safe)
    mysql_mutex_unlock(&table_share->LOCK_share);

  DBUG_RETURN(0);

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
  uint max_heap_table_size= thd->variables.max_heap_table_size;
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
bool Column_statistics_collected::add(ha_rows rowno)
{

  bool err= 0;
  if (column->is_null())
    nulls++;
  else
  {
    column_total_length+= column->value_length();
    if (min_value && column->update_min(min_value, rowno == nulls))
      set_not_null(COLUMN_STAT_MIN_VALUE);
    if (max_value && column->update_max(max_value, rowno == nulls))
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
void Column_statistics_collected::finish(ha_rows rows)
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
    ulonglong distincts;
    uint hist_size= count_distinct->get_hist_size();
    if (hist_size == 0)
      distincts= count_distinct->get_value();
    else
      distincts= count_distinct->get_value_with_histogram(rows - nulls);
    if (distincts)
    {
      val= (double) (rows - nulls) / distincts;
      set_avg_frequency(val); 
      set_not_null(COLUMN_STAT_AVG_FREQUENCY);
    }
    else
      hist_size= 0;
    histogram.set_size(hist_size);
    set_not_null(COLUMN_STAT_HIST_SIZE);
    if (hist_size && distincts)
    {
      set_not_null(COLUMN_STAT_HIST_TYPE);
      histogram.set_values(count_distinct->get_histogram());
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
  if (key_info->flags & HA_FULLTEXT)
    DBUG_RETURN(rc);

  Index_prefix_calc index_prefix_calc(table, key_info);

  DEBUG_SYNC(table->in_use, "statistics_collection_start1");
  DEBUG_SYNC(table->in_use, "statistics_collection_start2");

  if (index_prefix_calc.is_single_comp_pk)
  {
    index_prefix_calc.get_avg_frequency();
    DBUG_RETURN(rc);
  }

  table->key_read= 1;
  table->file->extra(HA_EXTRA_KEYREAD);

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
  table->key_read= 0;
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

  DBUG_ENTER("collect_statistics_for_table");

  table->collected_stats->cardinality_is_null= TRUE;
  table->collected_stats->cardinality= 0;

  for (field_ptr= table->field; *field_ptr; field_ptr++)
  {
    table_field= *field_ptr;   
    if (!bitmap_is_set(table->read_set, table_field->field_index))
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
      {
        if (rc == HA_ERR_RECORD_DELETED)
          continue;
        break;
      }

      for (field_ptr= table->field; *field_ptr; field_ptr++)
      {
        table_field= *field_ptr;
        if (!bitmap_is_set(table->read_set, table_field->field_index))
          continue;  
        if ((rc= table_field->collected_stats->add(rows)))
          break;
      }
      if (rc)
        break;
      rows++;
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
    table->collected_stats->cardinality= rows;
  }

  bitmap_clear_all(table->write_set);
  for (field_ptr= table->field; *field_ptr; field_ptr++)
  {
    table_field= *field_ptr;
    if (!bitmap_is_set(table->read_set, table_field->field_index))
      continue;
    bitmap_set_bit(table->write_set, table_field->field_index); 
    if (!rc)
      table_field->collected_stats->finish(rows);
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
  Open_tables_backup open_tables_backup;
  uint i;
  int err;
  enum_binlog_format save_binlog_format;
  int rc= 0;
  TABLE *stat_table;

  DBUG_ENTER("update_statistics_for_table");

  DEBUG_SYNC(thd, "statistics_update_start");

  if (open_stat_tables(thd, tables, &open_tables_backup, TRUE))
  {
    thd->clear_error();
    DBUG_RETURN(rc);
  }
   
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
    if (!bitmap_is_set(table->read_set, table_field->field_index))
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

  close_system_tables(thd, &open_tables_backup);

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
  Table_statistics *read_stats= table_share->stats_cb.table_stats;

  DBUG_ENTER("read_statistics_for_table");

  /* Read statistics from the statistical table table_stats */
  stat_table= stat_tables[TABLE_STAT].table;
  Table_stat table_stat(stat_table, table);
  table_stat.set_key_fields();
  table_stat.get_stat_values();
   
  /* Read statistics from the statistical table column_stats */
  stat_table= stat_tables[COLUMN_STAT].table;
  ulong total_hist_size= 0;
  Column_stat column_stat(stat_table, table);
  for (field_ptr= table_share->field; *field_ptr; field_ptr++)
  {
    table_field= *field_ptr;
    column_stat.set_key_fields(table_field);
    column_stat.get_stat_values();
    total_hist_size+= table_field->read_stats->histogram.get_size();
  }
  read_stats->total_hist_size= total_hist_size;

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
      
  table->stats_is_read= TRUE;

  DBUG_RETURN(0);
}


/**
  @brief
  Check whether any statistics is to be read for tables from a table list

  @param
  thd         The thread handle
  @param
  tables      The tables list for whose tables the check is to be done

  @details
  The function checks whether for any of the tables opened and locked for
  a statement statistics from statistical tables is needed to be read.

  @retval
  TRUE        statistics for any of the tables is needed to be read 
  @retval
  FALSE       Otherwise
*/

static
bool statistics_for_tables_is_needed(THD *thd, TABLE_LIST *tables)
{
  if (!tables)
    return FALSE;
  
  if (!statistics_for_command_is_needed(thd))
    return FALSE;

  /* 
    Do not read statistics for any query over non-user tables.
    If the query references some statistical tables, but not all 
    of them, reading the statistics may lead to a deadlock
  */ 
  for (TABLE_LIST *tl= tables; tl; tl= tl->next_global)
  {
    if (!tl->is_view_or_derived() && tl->table)
    {
      TABLE_SHARE *table_share= tl->table->s;
      if (table_share && 
          (table_share->table_category != TABLE_CATEGORY_USER ||
           table_share->tmp_table != NO_TMP_TABLE))
        return FALSE;
    }
  }

  for (TABLE_LIST *tl= tables; tl; tl= tl->next_global)
  {
    if (!tl->is_view_or_derived() && tl->table)
    {
      TABLE_SHARE *table_share= tl->table->s;
      if (table_share && 
          table_share->stats_cb.stats_can_be_read &&
          (!table_share->stats_cb.stats_is_read ||
           (!table_share->stats_cb.histograms_are_read &&
            thd->variables.optimizer_use_condition_selectivity > 3)))
        return TRUE;
      if (table_share->stats_cb.stats_is_read)
        tl->table->stats_is_read= TRUE;
      if (table_share->stats_cb.histograms_are_read)
        tl->table->histograms_are_read= TRUE;
    } 
  }

  return FALSE;
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
  TABLE_SHARE *table_share= table->s;
  
  DBUG_ENTER("read_histograms_for_table");

  if (!table_share->stats_cb.histograms_can_be_read)
  {
    (void) alloc_histograms_for_table_share(thd, table_share, FALSE);
  }
  if (table_share->stats_cb.histograms_can_be_read &&
      !table_share->stats_cb.histograms_are_read)
  {
    Field **field_ptr;
    uchar *histogram= table_share->stats_cb.table_stats->histograms;
    TABLE *stat_table= stat_tables[COLUMN_STAT].table;
    Column_stat column_stat(stat_table, table);
    for (field_ptr= table_share->field; *field_ptr; field_ptr++)
    {
      Field *table_field= *field_ptr;
      uint hist_size= table_field->read_stats->histogram.get_size();
      if (hist_size)
      {
        column_stat.set_key_fields(table_field);
        table_field->read_stats->histogram.set_values(histogram);
        column_stat.get_histogram_value();
        histogram+= hist_size;
      }
    }
  }
 
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
  TABLE_LIST stat_tables[STATISTICS_TABLES];
  Open_tables_backup open_tables_backup;

  DBUG_ENTER("read_statistics_for_tables_if_needed");

  DEBUG_SYNC(thd, "statistics_read_start");

  if (!statistics_for_tables_is_needed(thd, tables))
    DBUG_RETURN(0);

  if (open_stat_tables(thd, stat_tables, &open_tables_backup, FALSE))
  {
    thd->clear_error();
    DBUG_RETURN(1);
  }

  for (TABLE_LIST *tl= tables; tl; tl= tl->next_global)
  {
    if (!tl->is_view_or_derived() && tl->table)
    { 
      TABLE_SHARE *table_share= tl->table->s;
      if (table_share && 
          table_share->stats_cb.stats_can_be_read &&
	  !table_share->stats_cb.stats_is_read)
      {
        (void) read_statistics_for_table(thd, tl->table, stat_tables);
        table_share->stats_cb.stats_is_read= TRUE;
      }
      if (table_share->stats_cb.stats_is_read)
        tl->table->stats_is_read= TRUE;
      if (thd->variables.optimizer_use_condition_selectivity > 3 && 
          table_share && !table_share->stats_cb.histograms_are_read)
      {
        (void) read_histograms_for_table(thd, tl->table, stat_tables);
        table_share->stats_cb.histograms_are_read= TRUE;
      }
      if (table_share->stats_cb.stats_is_read)
        tl->table->histograms_are_read= TRUE;
    }
  }  

  close_system_tables(thd, &open_tables_backup);

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
  0         If all deletions are successful  
  @retval
  1         Otherwise

  @note
  The function is called when executing the statement DROP TABLE 'tab'.
*/

int delete_statistics_for_table(THD *thd, LEX_STRING *db, LEX_STRING *tab)
{
  int err;
  enum_binlog_format save_binlog_format;
  TABLE *stat_table;
  TABLE_LIST tables[STATISTICS_TABLES];
  Open_tables_backup open_tables_backup;
  int rc= 0;

  DBUG_ENTER("delete_statistics_for_table");
   
  if (open_stat_tables(thd, tables, &open_tables_backup, TRUE))
  {
    thd->clear_error();
    DBUG_RETURN(rc);
  }

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

  thd->restore_stmt_binlog_format(save_binlog_format);

  close_system_tables(thd, &open_tables_backup);

  DBUG_RETURN(rc);
}


/**
  @brief
  Delete statistics on a column of the specified table

  @param
  thd         The thread handle
  @param
  tab         The table the column belongs to
  @param
  col         The field of the column whose statistics is to be deleted

  @details
  The function delete statistics on the column 'col' belonging to the table 
  'tab' from the statistical table column_stats. 

  @retval
  0         If the deletion is successful  
  @retval
  1         Otherwise

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
  Open_tables_backup open_tables_backup;
  int rc= 0;

  DBUG_ENTER("delete_statistics_for_column");
   
  if (open_single_stat_table(thd, &tables, &stat_table_name[1],
                             &open_tables_backup, TRUE))
  {
    thd->clear_error();
    DBUG_RETURN(rc);
  }

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

  close_system_tables(thd, &open_tables_backup);

  DBUG_RETURN(rc);
}


/**
  @brief
  Delete statistics on an index of the specified table

  @param
  thd         The thread handle
  @param
  tab         The table the index belongs to
  @param
  key_info    The descriptor of the index whose statistics is to be deleted
  @param
  ext_prefixes_only  Delete statistics only on the index prefixes extended by
                     the components of the primary key 

  @details
  The function delete statistics on the index  specified by 'key_info'
  defined on the table 'tab' from the statistical table index_stats.

  @retval
  0         If the deletion is successful  
  @retval
  1         Otherwise

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
  Open_tables_backup open_tables_backup;
  int rc= 0;

  DBUG_ENTER("delete_statistics_for_index");
   
  if (open_single_stat_table(thd, &tables, &stat_table_name[2],
			     &open_tables_backup, TRUE))
  {
    thd->clear_error();
    DBUG_RETURN(rc);
  }

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

  thd->restore_stmt_binlog_format(save_binlog_format);

  close_system_tables(thd, &open_tables_backup);

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

int rename_table_in_stat_tables(THD *thd, LEX_STRING *db, LEX_STRING *tab,
                                LEX_STRING *new_db, LEX_STRING *new_tab)
{
  int err;
  enum_binlog_format save_binlog_format;
  TABLE *stat_table;
  TABLE_LIST tables[STATISTICS_TABLES];
  Open_tables_backup open_tables_backup;
  int rc= 0;

  DBUG_ENTER("rename_table_in_stat_tables");
   
  if (open_stat_tables(thd, tables, &open_tables_backup, TRUE))
  {
    thd->clear_error();
    DBUG_RETURN(rc);
  }

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

  close_system_tables(thd, &open_tables_backup);

  DBUG_RETURN(rc);
}


/**
  @brief
  Rename a column in the statistical table column_stats

  @param
  thd         The thread handle
  @param
  tab         The table the column belongs to
  @param
  col         The column to be renamed
  @param
  new_name    The new column name

  @details
  The function replaces the name of the column 'col' belonging to the table 
  'tab' for 'new_name' in the statistical table column_stats.

  @retval
  0         If all updates of the table name are successful  
  @retval
  1         Otherwise

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
  Open_tables_backup open_tables_backup;
  int rc= 0;

  DBUG_ENTER("rename_column_in_stat_tables");
  
  if (tab->s->tmp_table != NO_TMP_TABLE)
    DBUG_RETURN(0);

  if (open_single_stat_table(thd, &tables, &stat_table_name[1],
                             &open_tables_backup, TRUE))
  {
    thd->clear_error();
    DBUG_RETURN(rc);
  }

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

  close_system_tables(thd, &open_tables_backup);

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
  Use_stat_tables_mode use_stat_table_mode= get_use_stat_tables_mode(thd);
  table->used_stat_records= 
    (use_stat_table_mode <= COMPLEMENTARY ||
     !table->stats_is_read || read_stats->cardinality_is_null) ?
    table->file->stats.records : read_stats->cardinality;
  KEY *key_info, *key_info_end;
  for (key_info= table->key_info, key_info_end= key_info+table->s->keys;
       key_info < key_info_end; key_info++)
  {
    key_info->is_statistics_from_stat_tables=
      (use_stat_table_mode > COMPLEMENTARY &&
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
    res= table->stat_records();
    return res;
  }
 
  Column_statistics *col_stats= table->s->field[field->field_index]->read_stats;

  if (!col_stats)
    res= table->stat_records();
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
  The required estimate of the rows in the column range
*/

double get_column_range_cardinality(Field *field,
                                    key_range *min_endp,
                                    key_range *max_endp,
                                    uint range_flag)
{
  double res;
  TABLE *table= field->table;
  Column_statistics *col_stats= table->field[field->field_index]->read_stats;
  double tab_records= table->stat_records();

  if (!col_stats)
    return tab_records;

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
      /*
        psergey-todo: what does check for min_value, max_value mean? 
          min/max_value are set to NULL in alloc_statistics_for_table() and
          alloc_statistics_for_table_share().  Both functions will immediately
          call create_min_max_statistical_fields_for_table and 
          create_min_max_statistical_fields_for_table_share() respectively,
          which will set min/max_value to be valid pointers, unless OOM
          occurs.
      */
      if (avg_frequency > 1.0 + 0.000001 && 
          col_stats->min_value && col_stats->max_value)
      {
        Histogram *hist= &col_stats->histogram;
        if (hist->is_available())
        {
          store_key_image_to_rec(field, (uchar *) min_endp->key,
                                 field->key_length());
          double pos= field->pos_in_interval(col_stats->min_value,
                                             col_stats->max_value);
          res= col_non_nulls * 
	       hist->point_selectivity(pos,
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
    if (col_stats->min_value && col_stats->max_value)
    {
      double sel, min_mp_pos, max_mp_pos;

      if (min_endp && !(field->null_ptr && min_endp->key[0]))
      {
        store_key_image_to_rec(field, (uchar *) min_endp->key,
                               field->key_length());
        min_mp_pos= field->pos_in_interval(col_stats->min_value,
                                           col_stats->max_value);
      }
      else
        min_mp_pos= 0.0;
      if (max_endp)
      {
        store_key_image_to_rec(field, (uchar *) max_endp->key,
                               field->key_length());
        max_mp_pos= field->pos_in_interval(col_stats->min_value,
                                           col_stats->max_value);
      }
      else
        max_mp_pos= 1.0;

      Histogram *hist= &col_stats->histogram;
      if (!hist->is_available())
        sel= (max_mp_pos - min_mp_pos);
      else
        sel= hist->range_selectivity(min_mp_pos, max_mp_pos);
      res= col_non_nulls * sel;
      set_if_bigger(res, col_stats->get_avg_frequency());
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
  
  @param pos      Position of the "const" between column's min_value and 
                  max_value.  This is a number in [0..1] range.
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

double Histogram::point_selectivity(double pos, double avg_sel)
{
  double sel;
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

      Histogram buckets have the same numbers of rows, but they cover
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

