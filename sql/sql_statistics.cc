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

#include "sql_base.h"
#include "key.h"
#include "sql_statistics.h"

/*
  The system variable 'optimizer_use_stat_tables' can take one of the
  following values:
  "never", "complementary", "preferably", "exclusively". 
  If the values of the variable 'optimizer_use_stat_tables' is set to
  "never then any statistical data from  the persistent statistical tables
  is ignored by the optimizer.
  If the value of the variable 'optimizer_use_stat_tables' is set to
  "complementary" then a particular statistical characteristic is used
  by the optimizer only if the database engine does not provide similar
  statistics. For example, 'nulls_ratio' for table columns  currently 
  are not provided by any engine. So optimizer uses this statistical data
  from the  statistical tables. At the same time it does not use 
  'avg_frequency' for any index prefix from the statistical tables since
  the a similar statistical characteristic 'records_per_key' can be
  requested from the database engine.
  If the value the variable 'optimizer_use_stat_tables' is set to
  "preferably" the optimizer uses a particular statistical data only if
  it can't be found in the statistical data.
  If the value of the variable 'optimizer_use_stat_tables' is set to
  "exclusively" the optimizer never uses statistical data that can be
  returned by the database engine Only statistical data from the
  statistical tables is used.
  If an ANALYZE command is executed then it results in collecting
  statistical data for the tables specified by the command and storing
  the collected statistics in the persistent statistical tables only
  when the value of the variable 'optimizer_use_stat_tables' is not
  equal to "never".
*/ 
   
/* Currently there are only 3 persistent statistical tables */
static const uint STATISTICS_TABLES= 3;

/* 
  The names of the statistical tables in this list must correspond the
  definitions of the tables in the file ../scripts/mysql_system_tables.sql
*/
static const char *STAT_TABLE_NAME[STATISTICS_TABLES]=
{
  "table_stat",
  "column_stat",
  "index_stat"
};

/**
  @details
  The function builds a list of TABLE_LIST elements for system statistical
  tables using array of TABLE_LIST passed as a parameter. 
  The lock type of each element is set to TL_READ if for_write = FALSE,
  otherwise it is set to TL_WRITE.
*/
inline void init_table_list_for_stat_tables(TABLE_LIST *tables, bool for_write)
{
  uint i;

  bzero((char *) &tables[0], sizeof(TABLE_LIST) * STATISTICS_TABLES);

  for (i= 0; i < STATISTICS_TABLES; i++)
  {
    tables[i].db= (char*) "mysql";
    tables[i].table_name= (char *) STAT_TABLE_NAME[i];
    tables[i].alias= tables[i].table_name;
    tables[i].lock_type= for_write ? TL_WRITE : TL_READ;
    if (i < STATISTICS_TABLES - 1)
    tables[i].next_global= tables[i].next_local=
      tables[i].next_name_resolution_table= &tables[i+1];
  }
}

/**
  @details
  The function sets null bits stored in the bitmap table_field->write_stat
  for all statistical values collected for a column.
*/
inline void set_nulls_for_write_column_stat_values(Field *table_field)
{
  table_field->write_stat.column_stat_nulls= 
    ((1 << (COLUMN_STAT_AVG_FREQUENCY-COLUMN_STAT_COLUMN_NAME))-1) <<
    (COLUMN_STAT_COLUMN_NAME+1);
}

/** 
  @details
  The function sets null bits stored in the bitmap table_field->read_stat
  for all statistical values collected for a column.
*/
inline void set_nulls_for_read_column_stat_values(Field *table_field)
{
  table_field->read_stat.column_stat_nulls= 
    ((1 << (COLUMN_STAT_AVG_FREQUENCY-COLUMN_STAT_COLUMN_NAME))-1) <<
    (COLUMN_STAT_COLUMN_NAME+1);
}

/**
  @details
  The function removes the null bit stored in the bitmap
  table_field->write_stat for the statistical value collected
  on the statistical column number stat_field_no.  
*/
inline void set_not_null_for_write_column_stat_value(Field *table_field,
                                                     uint stat_field_no)
{
  table_field->write_stat.column_stat_nulls&= ~(1 << stat_field_no);
}  
  
/** 
  @details
  The function removes the null bit stored in the bitmap
  table_field->read_stat for the statistical value collected
  on the statistical column number stat_field_no.  
*/
inline void set_not_null_for_read_column_stat_value(Field *table_field,
                                                    uint stat_field_no)
{
  table_field->read_stat.column_stat_nulls&= ~(1 << stat_field_no);
}

/** 
  @details
  The function checks the null bit stored in the bitmap
  table_field->read_stat for the statistical value collected
  on the statistical column number stat_field_no.  
*/
inline bool check_null_for_write_column_stat_value(Field *table_field,
                                                   uint stat_field_no)
{
  return table_field->write_stat.column_stat_nulls & (1 << stat_field_no);
}

/*
  Stat_table is the base class for classes Table_stat, Column_stat and
  Index_stat. The methods of these classes allow us table to read
  statistical data from statistical tables and write collected statistical
  data into statistical data. Objects of the classes Table_stat, Column_stat
  and Index stat are used for reading/writing statistics from/into 
  persistent tables table_stat, column_stat and index_stat correspondingly.
  These tables are stored in the system database 'mysql'.

  Statistics is read and written always for a given database table t. When
  an  object of any of these classes is created a pointer to the TABLE
  structure for this database table is passed as a parameter to the constructor
  of the object. The other parameter is a pointer to the TABLE structure for
  the corresponding statistical table st. So construction of an object to 
  read/write statistical data on table t from/into statistical table st 
  requires both table t and st to be opened.

  Reading/writing statistical data from/into a statistical table is always
  performed by key. At the moment there is only one key defined for each
  statistical table and this key is primary.
  The primary key for the table table_stat is built as (db_name, table_name).
  The primary key for the table column_stat is built as (db_name, table_name,
  column_name).
  The primary key for the table index_stat is built as (db_name, table_name,
  index_name, prefix_arity).

  Reading statistical data from a statistical table is performed by the 
  following pattern. First a table dependent method sets the values of the
  the fields that comprise the lookup key. Then an implementation of the 
  method get_stat_values() declared in Stat_table as a pure virtual method
  finds the row from the statistical table by the set key. If the row is
  found the values of statistical fields are read from this row and are
  distributed in the internal structures.

  Let's assume the statistical data is read for table t from database db.

  When statistical data is searched in the table table_stat first 
  Table_stat::set_key_fields() should set the fields of db_name and
  table_name. Then get_stat_values looks for a row by the set key value,
  and, if the row is found, reads the value from the column
  table_stat.cardinality into the field read_stat.cardinality of the TABLE
  structure for table t and sets the value of read_stat.cardinality_is_null
  from this structure to FALSE. If the value of the 'cardinality' column
  in the row is null or if no row is found read_stat.cardinality_is_null
  is set to TRUE.

  When statistical data is searched in the table column_stat first
  Column_stat::set_key_fields() should set the fields of db_name, table_name
  and column_name with column_name taken out of the only parameter f of the
  Field* type passed to this method. After this get_stat_values looks
  for a row by the set key value. If the row is found the values of statistical 
  data columns min_value, max_value, nulls_ratio, avg_length, avg_frequency
  are read into internal structures. Values of nulls_ratio, avg_length,
  avg_frequency are read into the corresponding fields of the read_stat
  structure from the Field object f, while values from min_value and max_value
  are copied into the min_value and  max_value record buffers attached to the
  TABLE structure for table t.
  If the value of a statistical column in the found row is null, then the
  corresponding flag in the f->read_stat.column_stat_nulls bitmap is set off.
  Otherwise the flag is set on. If no row is found for the column the all flags
  in f->column_stat_nulls are set off.
  
  When statistical data is searched in the table index_stat first
  Index_stat::set_key_fields() has to be called to set the fields of db_name,
  table_name, index_name and prefix_arity. The value of index_name is extracted
  from the first parameter key_info of the KEY* type passed to the method.
  This parameter  specifies the index of interest idx. The second parameter
  passed to the method specifies the arity k of the index prefix for which
  statistical data is to be read. E.g. if the index idx consists of 3
  components (p1,p2,p3) the table  index_stat usually will contain 3 rows for
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
  the colected statistical data from internal structures are to be written
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
  
  KEY *stat_key_info;   /* Structure for the index to access stat_table */
  uint stat_key_length; /* Length of the key to access stat_table */   
  uchar *record[2];     /* Record buffers used to access/update stat_table */
  uint stat_key_idx;    /* The number of the key to access stat_table */

protected:
  /* Statistical table to read statistics from or to update */
  TABLE *stat_table;
  
  /* Table for which statistical data is read / updated */
  TABLE *table;     
  char *db_name;        /* Name of the database containing 'table' */ 
  uint db_name_len;     /* Length of db_name */
  char *table_name;     /* Name of the table 'table' */
  uint table_name_len;  /* Name of table_name */

public:

  /*
    @details
    This constructor has to be called by any constructor of the derived
    classes. The constructor 'tunes' the private and protected members of
    the constructed object to the statistical table 'stat_table' with the
    statistical data of our interest and to the table 'tab' for which this
    statistics has been collected.
  */  
  Stat_table(TABLE *stat, TABLE *tab) :stat_table(stat), table(tab)
  {
    stat_file= stat_table->file;
    /* Currently any statistical table has only one key */
    stat_key_idx= 0;
    stat_key_info= &stat_table->key_info[stat_key_idx];
    stat_key_length= stat_key_info->key_length;
    record[0]= stat_table->record[0];
    record[1]= stat_table->record[1];
    db_name= table->s->db.str;
    db_name_len= table->s->db.length;
    table_name= table->s->table_name.str;
    table_name_len= table->s->table_name.length;    
  }

  virtual ~Stat_table() {}

  /*
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

  /*
    @brief
    Read statistical data from fields of the statistical table
   
    @details
    This is a purely virtual method.
    The implementation for any derived read shall read the appropriate
    statistical data from the corresponding fields of stat_table.    
  */      
  virtual void get_stat_values()= 0;

  /*
    @breif
    Find a record by key in the statistical table

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

  /*
    @breif
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
    int err;
    if (find_stat())
    {    
      store_record(stat_table, record[1]);
      store_stat_fields();
      if ((err= stat_file->ha_update_row(record[1], record[0])) &&
           err != HA_ERR_RECORD_IS_THE_SAME)
        return TRUE;
    }
    else
    {
      store_stat_fields();
      if ((err= stat_file->ha_write_row(record[0])))
	return TRUE;
    } 
    return FALSE;
  }
};


/*
  An object of the class Table_stat is created to read statistical
  data on tables from the statistical table table_stat or to update
  table_stat with such statistical data.
  Rows from the statistical table are read and updated always by
  primary key. 
*/

class Table_stat: public Stat_table
{
private:
  Field *db_name_field;     /* Field for the column table_stat.db_name */
  Field *table_name_field;  /* Field for the column table_stat.table_name */

public:

  /*
    @details
    The constructor 'tunes' the private and protected members of the
    constructed object for the statistical table table_stat to read/update
    statistics on table 'tab'. The TABLE structure for the table table_stat
    must be passed as a value for the parameter 'stat'.
  */
  Table_stat(TABLE *stat, TABLE *tab) :Stat_table(stat, tab)
  {
    db_name_field= stat_table->field[TABLE_STAT_DB_NAME];
    table_name_field= stat_table->field[TABLE_STAT_TABLE_NAME];
  }

  /* 
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
    db_name_field->store(db_name, db_name_len, &my_charset_bin);
    table_name_field->store(table_name, table_name_len, &my_charset_bin);
  }

  /* 
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
    if (table->write_stat.cardinality_is_null)
      stat_field->set_null();
    else
    {
      stat_field->set_notnull();
      stat_field->store(table->write_stat.cardinality);
    }
  }

  /* 
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
    table->read_stat.cardinality_is_null= TRUE;
    table->read_stat.cardinality= 0;
    if (find_stat())
    {
      Field *stat_field= stat_table->field[TABLE_STAT_CARDINALITY];
      if (!stat_field->is_null())
      {
        table->read_stat.cardinality_is_null= FALSE;
        table->read_stat.cardinality= stat_field->val_int();
      }
    }
  } 

};


/*
  An object of the class Column_stat is created to read statistical data
  on table columns from the statistical table column_stat or to update
  column_stat with such statistical data.
  Rows from the statistical table are read and updated always by 
  primary key.
*/ 

class Column_stat: public Stat_table
{
private:
  Field *db_name_field;     /* Field for the column column_stat.db_name */
  Field *table_name_field;  /* Field for the column column_stat.table_name */
  Field *column_name_field; /* Field for the column column_stat.column_name */

  Field *table_field;  /* Field from 'table' to read /update statistics on */

public:

  /*
    @details
    The constructor 'tunes' the private and protected members of the
    constructed object for the statistical table column_stat to read/update
    statistics on fields of the table 'tab'. The TABLE structure for the table
    column_stat must be passed as a value for the parameter 'stat'.
  */
  Column_stat(TABLE *stat, TABLE *tab) :Stat_table(stat, tab)
  {
    db_name_field= stat_table->field[COLUMN_STAT_DB_NAME];
    table_name_field= stat_table->field[COLUMN_STAT_TABLE_NAME];
    column_name_field= stat_table->field[COLUMN_STAT_COLUMN_NAME];
  } 

  /* 
    @brief
    Set the key fields for the statistical table column_stat

    @param
    column    Field for the 'table' column to read/update statistics on

    @details
    The function sets the values of the fields db_name, table_name and
    column_name in the record buffer for the statistical table column_stat.
    These fields comprise the primary key for the table.

    @note
    The function is supposed to be called before any use of the  
    method find_stat for an object of the Column_stat class.
  */
  void set_key_fields(Field *column)
  {
    db_name_field->store(db_name, db_name_len, &my_charset_bin);
    table_name_field->store(table_name, table_name_len, &my_charset_bin);
    table_field= column;
    const char *column_name= column->field_name;
    column_name_field->store(column_name, strlen(column_name), &my_charset_bin);  
  }

  /* 
    @brief
    Store statistical data into statistical fields of column_stat

    @details
    This implementation of a purely virtual method sets the value of the
    columns 'min_value', 'max_value', 'nulls_ratio', 'avg_length' and
    'avg_frequency' of the stistical table columns_stat according to the
    contents of the bitmap write_stat.column_stat_nulls and the values
    of the fields min_value, max_value, nulls_ratio, avg_length and
    avg_frequency of the structure write_stat from the Field structure
    for the field 'table_field'.
    The value of the k-th column in the table columns_stat is set to NULL
    if the k-th bit in the bitmap 'column_stat_nulls' is set to 1. 

    @note
    A value from the field min_value/max_value is always converted
    into a utf8 string. If the length of the column 'min_value'/'max_value'
    is less than the length of the string the string is trimmed to fit the
    length of the column. 
  */    
  void store_stat_fields()
  {
    char buff[MAX_FIELD_WIDTH];
    String val(buff, sizeof(buff), &my_charset_utf8_bin);

    for (uint i= COLUMN_STAT_MIN_VALUE; i <= COLUMN_STAT_AVG_FREQUENCY; i++)
    {  
      Field *stat_field= stat_table->field[i];
      if (check_null_for_write_column_stat_value(table_field, i))
        stat_field->set_null();
      else
      {
        stat_field->set_notnull();
        switch (i) {
        case COLUMN_STAT_MIN_VALUE:
          if (table_field->type() == MYSQL_TYPE_BIT)
            stat_field->store(table_field->write_stat.min_value->val_int());
          else
          {
            table_field->write_stat.min_value->val_str(&val);
            stat_field->store(val.ptr(), val.length(), &my_charset_utf8_bin);
          }
          break;
        case COLUMN_STAT_MAX_VALUE:
          if (table_field->type() == MYSQL_TYPE_BIT)
            stat_field->store(table_field->write_stat.max_value->val_int());
          else
          {
            table_field->write_stat.max_value->val_str(&val);
            stat_field->store(val.ptr(), val.length(), &my_charset_utf8_bin);
          }
          break;
        case COLUMN_STAT_NULLS_RATIO:
          stat_field->store(table_field->write_stat.nulls_ratio);
          break;
        case COLUMN_STAT_AVG_LENGTH:
          stat_field->store(table_field->write_stat.avg_length);
          break;
        case COLUMN_STAT_AVG_FREQUENCY:
          stat_field->store(table_field->write_stat.avg_frequency);
          break;            
        }
      }
    }
  }

  /* 
    @brief
    Read statistical data from statistical fields of column_stat

    @details
    This implementation of a purely virtual method first looks for a record
    the statistical table column_stat by its primary key set the record
    buffer with the help of Column_stat::set_key_fields. Then, if the row is
    found, the function reads the values of the columns 'min_value',
    'max_value', 'nulls_ratio', 'avg_length' and 'avg_frequency' of the
    table column_stat and sets accordingly the value of the bitmap 
    read_stat.column_stat_nulls' and the values of the fields min_value,
    max_value, nulls_ratio, avg_length and avg_frequency of the structure
    read_stat from the Field structure for the field 'table_field'.
  */    
  void get_stat_values()
  {
    set_nulls_for_read_column_stat_values(table_field);

    if (table_field->read_stat.min_value)
      table_field->read_stat.min_value->set_null();
    if (table_field->read_stat.max_value)
      table_field->read_stat.max_value->set_null();

    if (find_stat())
    {
      char buff[MAX_FIELD_WIDTH];
      String val(buff, sizeof(buff), &my_charset_utf8_bin);

      for (uint i= COLUMN_STAT_MIN_VALUE; i <= COLUMN_STAT_AVG_FREQUENCY; i++)
      {  
        Field *stat_field= stat_table->field[i];

        if (!stat_field->is_null() &&
            (i > COLUMN_STAT_MAX_VALUE ||
             (i == COLUMN_STAT_MIN_VALUE && table_field->read_stat.min_value) ||
             (i == COLUMN_STAT_MAX_VALUE && table_field->read_stat.max_value)))
        {
          set_not_null_for_read_column_stat_value(table_field, i);

          switch (i) {
          case COLUMN_STAT_MIN_VALUE:
            stat_field->val_str(&val);
            table_field->read_stat.min_value->store(val.ptr(), val.length(),
                                                     &my_charset_utf8_bin);
            break;
          case COLUMN_STAT_MAX_VALUE:
            stat_field->val_str(&val);
            table_field->read_stat.max_value->store(val.ptr(), val.length(),
                                                    &my_charset_utf8_bin);
            break;
          case COLUMN_STAT_NULLS_RATIO:
            table_field->read_stat.nulls_ratio= stat_field->val_real();
            break;
          case COLUMN_STAT_AVG_LENGTH:
            table_field->read_stat.avg_length= stat_field->val_real();
            break;
          case COLUMN_STAT_AVG_FREQUENCY:
            table_field->read_stat.avg_frequency= stat_field->val_real();
            break;            
          }
        }
      }
    }
  }

};


/*
  An object of the class Index_stat is created to read statistical
  data on index prefixes from the statistical table index_stat or
  to update index_stat with such statistical data.
  Rows from the statistical table are read and updated always by 
  primary key.
*/ 

class Index_stat: public Stat_table
{
private:
  Field *db_name_field;      /* Field for the column index_stat.db_name */
  Field *table_name_field;   /* Field for the column index_stat.table_name */
  Field *index_name_field;   /* Field for the column index_stat.table_name */
  Field *prefix_arity_field; /* Field for the column index_stat.prefix_arity */

  KEY *table_key_info;  /* Info on the index to read/update statistics on */
  uint prefix_arity; /* Number of components of the index prefix of interest */

public:

  /*
    @details
    The constructor 'tunes' the private and protected members of the
    constructed object for the statistical table index_stat to read/update
    statistics on prefixes of different indexes of the table 'tab'.
    The TABLE structure for the table index_stat must be passed as a value
    for the parameter 'stat'.
  */
  Index_stat(TABLE *stat, TABLE *tab) :Stat_table(stat, tab)
  {
    db_name_field= stat_table->field[INDEX_STAT_DB_NAME];
    table_name_field= stat_table->field[INDEX_STAT_TABLE_NAME];
    index_name_field= stat_table->field[INDEX_STAT_INDEX_NAME];
    prefix_arity_field= stat_table->field[INDEX_STAT_PREFIX_ARITY];
    
  } 

  /* 
    @brief
    Set the key fields for the statistical table index_stat

    @param
    index_info   Info for the index of 'table' to read/update statistics on
    @param
    index_prefix_arity Number of components in the index prefix of interest


    @details
    The function sets the values of the fields db_name, table_name and
    index_name, prefix_arity in the record buffer for the statistical
    table index_stat. These fields comprise the primary key for the table. 

    @note
    The function is supposed to be called before any use of the  
    method find_stat for an object of the Index_stat class.
  */
    void set_key_fields(KEY *index_info, uint index_prefix_arity)
  {
    db_name_field->store(db_name, db_name_len, &my_charset_bin);
    table_name_field->store(table_name, table_name_len, &my_charset_bin);
    table_key_info= index_info;
    char *index_name= index_info->name;
    index_name_field->store(index_name, strlen(index_name), &my_charset_bin);
    prefix_arity= index_prefix_arity; 
    prefix_arity_field->store(index_prefix_arity, TRUE);  
  }

  /* 
    @brief
    Store statistical data into statistical fields of tableindex_stat

    @details
    This implementation of a purely virtual method sets the value of the
    column 'avg_frequency' of the statistical table index_stat according to
    the value of write_stat.avg_frequency[Index_stat::prefix_arity]
    from the KEY_INFO structure 'table_key_info'.
    If the value of write_stat. avg_frequency[Index_stat::prefix_arity] is
    equal  to 0, the value of the column is set to NULL.
  */    
  void store_stat_fields()
  {
    Field *stat_field= stat_table->field[INDEX_STAT_AVG_FREQUENCY]; 
    double avg_frequency=
             table_key_info->write_stat.avg_frequency[prefix_arity-1];
    if (avg_frequency == 0)
      stat_field->set_null();
    else
    {
      stat_field->set_notnull();
      stat_field->store(avg_frequency);
    }
  }

  /* 
    @brief
    Read statistical data from statistical fields of index_stat

    @details
    This implementation of a purely virtual method first looks for a record the
    statistical table index_stat by its primary key set the record buffer with
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
    table_key_info->read_stat.avg_frequency[prefix_arity-1]= avg_frequency;
  }  

};


/*
  The class Count_distinct_field is a helper class used to calculate
  the number of distinct values for a column. The class employs the
  Unique class for this purpose.
  The class Count_distinct_field is used only by the function
  collect_statistics_from_table to calculate the values for 
  column avg_frequency of the statistical table column_stat.
*/
    
class Count_distinct_field: public Sql_alloc
{
protected:

  /* Field for which the number of distinct values is to be find out */
  Field *table_field;  
  Unique *tree;       /* The helper object to contain distinct values */
  uint tree_key_length; /* The length of the keys for the elements of 'tree */

public:

  /*
    @param
    field               Field for which the number of distinct values is 
                        to be find out
    @param
    max_heap_table_size The linit for the memory used by the RB tree container
                        of the constructed Unique object 'tree' 

    @details
    The constructor sets the values of 'table_field' and 'tree_key_length',
    and then calls the 'new' operation to create a Unique object for 'tree'.
    The type of 'field' and the value max_heap_table_size of determine the set
    of the parameters to be passed to the constructor of the Unique object. 
  */  
  Count_distinct_field(Field *field, uint max_heap_table_size)
  {
    qsort_cmp2 compare_key;
    void* cmp_arg;
    enum enum_field_types f_type= field->type();

    table_field= field;
    tree_key_length= field->pack_length();

    if ((f_type == MYSQL_TYPE_VARCHAR) ||
	(!field->binary() && (f_type == MYSQL_TYPE_STRING ||
                              f_type == MYSQL_TYPE_VAR_STRING)))
    {
      compare_key= (qsort_cmp2) simple_str_key_cmp;
      cmp_arg= (void*) field;      
    }
    else
    {
      cmp_arg= (void*) &tree_key_length;
      compare_key= (qsort_cmp2) simple_raw_key_cmp;
    }
        
    tree= new Unique(compare_key, cmp_arg,
                     tree_key_length, max_heap_table_size);
  }

  virtual ~Count_distinct_field()
  {
    delete tree;
    tree= NULL;
  }

  /* 
    @brief
    Check whether the Unique object tree has been succesfully created
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
    tree->walk(count_distinct_walk, (void*) &count);
    return count;
  }
};


/* 
  The class Count_distinct_field_bit is derived from the class 
  Count_distinct_field to be used only for fields of the MYSQL_TYPE_BIT type.
  The class provides a different implementation for the method add 
*/

class Count_distinct_field_bit: public Count_distinct_field
{
public:
  Count_distinct_field_bit(Field *field, uint max_heap_table_size)
    :Count_distinct_field(field, max_heap_table_size) {}
  bool add()
  {
    longlong val= table_field->val_int();   
    return tree->unique_add(&val);
  }
};


/* 
  The class Index_prefix_calc is a helper class used to calculate the values
  for the column 'avg_frequency' of the statistical table index_stat.
  For any table t from the database db and any k-component prefix of the
  index i for this table the row from index_stat with the primary key
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
    /* The values of the last encoutered k-component prefix */
    Cached_item *last_prefix;
  };

  /* 
    Array of structures used to calculate 'avg_frequency' for different
    prefixes of the index i
  */   
  Prefix_calc_state *calc_state;
    
public:
  Index_prefix_calc(TABLE *table, KEY *key_info)
    : index_table(table), index_info(key_info)
  {
    uint i;
    Prefix_calc_state *state;
    uint key_parts= key_info->key_parts;
    empty= TRUE;
    prefixes= 0;
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

  /* 
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

  /*
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
    for (i= 0, state= calc_state; i < prefixes; i++, state++)
    {
      if (i < prefixes)
      {
        index_info->write_stat.avg_frequency[i]=
          state->prefix_count == 0 ? 0 :
          (double) state->entry_count / state->prefix_count;
      }
    }
  }       
};


/**
  @brief 
  Create fields for min/max values to collect/read column statistics

  @param
  table       Table the fields are created for
  @param
  for_write   Those fields are created that are used to collect statistics

  @note
  The function first allocates record buffers to store min/max values
  for 'table's fields. Then for each table field f it creates Field structures
  that points to these buffers rather that to the record buffer as the
  Field object for f does. The pointers of the created fields are placed
  either in the write_stat or in the read_stat structure of the Field
  object for f, depending on the value of the 'for_write' parameter.

  @note 
  The buffers allocated when min/max values are used to read statistics
  from the persistent statistical tables differ from those buffers that
  are used when statistics on min/max values for column is collected.
  The same is true for the fields created for min/max values.  
*/      

static
void create_min_max_stistical_fields(TABLE *table, bool for_write)
{
  Field *table_field;
  Field **field_ptr;
  uchar *record;
  uint rec_buff_length= table->s->rec_buff_length;

  for (field_ptr= table->field; *field_ptr; field_ptr++) 
  {
    table_field= *field_ptr;
    if (for_write)
      table_field->write_stat.max_value=
        table_field->write_stat.min_value= NULL;
    else
      table_field->read_stat.max_value=
        table_field->read_stat.min_value= NULL;
  }

  if ((record= (uchar *) alloc_root(&table->mem_root, 2*rec_buff_length)))
  {
    for (uint i=0; i < 2; i++, record+= rec_buff_length)
    {
      for (field_ptr= table->field; *field_ptr; field_ptr++) 
      {
        Field *fld;
        table_field= *field_ptr;
        my_ptrdiff_t diff= record-table->record[0];
        if (!(fld= table_field->clone(&table->mem_root, table, diff, TRUE)))
          continue;
        if (i == 0)
        {
          if (for_write)
            table_field->write_stat.min_value= fld;
          else
            table_field->read_stat.min_value= fld;
        }
        else
        {
          if (for_write)
            table_field->write_stat.max_value= fld;
          else
            table_field->read_stat.max_value= fld;
        }
      }
    }
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
int collect_statistics_for_index(TABLE *table, uint index)
{
  int rc= 0;
  KEY *key_info= &table->key_info[index];
  ha_rows rows= 0;
  Index_prefix_calc index_prefix_calc(table, key_info);
  DBUG_ENTER("collect_statistics_for_index");

  table->key_read= 1;
  table->file->extra(HA_EXTRA_KEYREAD);

  table->file->ha_index_init(index, TRUE);
  rc= table->file->ha_index_first(table->record[0]);
  while (rc != HA_ERR_END_OF_FILE)
  {
    if (rc)
      break;
    rows++;
    index_prefix_calc.add();
    rc= table->file->ha_index_next(table->record[0]);
  }
  table->key_read= 0;
  table->file->ha_index_end();

  rc= (rc == HA_ERR_END_OF_FILE) ? 0 : 1;

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
  to be saved in the statistical tables table_stat and column_stat. To do this
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

  table->write_stat.cardinality_is_null= TRUE;
  table->write_stat.cardinality= 0;
 
  create_min_max_stistical_fields(table, TRUE);

  for (field_ptr= table->field; *field_ptr; field_ptr++)
  {
    table_field= *field_ptr;
    uint max_heap_table_size= thd->variables.max_heap_table_size;
    set_nulls_for_write_column_stat_values(table_field);
    table_field->nulls= 0;
    table_field->column_total_length= 0;
    if (table_field->flags & BLOB_FLAG)
      table_field->count_distinct= NULL;
    else
    {
      table_field->count_distinct=
        table_field->type() == MYSQL_TYPE_BIT ?
        new Count_distinct_field_bit(table_field, max_heap_table_size) :
        new Count_distinct_field(table_field, max_heap_table_size);
    }
    if (table_field->count_distinct && 
       !table_field->count_distinct->exists())
      table_field->count_distinct= NULL;
  }

  bitmap_set_all(table->read_set);

  /* Perform a full table scan to collect statistics on 'table's columns */
  if (!(rc= file->ha_rnd_init(FALSE)))
  {  
    while ((rc= file->ha_rnd_next(table->record[0])) != HA_ERR_END_OF_FILE)
    {
      if (rc)
        break;

      for (field_ptr= table->field; *field_ptr; field_ptr++)
      {
        table_field= *field_ptr;
        if (table_field->is_null())
          table_field->nulls++;
        else
        {
          table_field->column_total_length+= table_field->value_length();
          if (table_field->write_stat.min_value &&
              table_field->update_min(table_field->write_stat.min_value,
                                      rows == table_field->nulls))
            set_not_null_for_write_column_stat_value(table_field,
                                                     COLUMN_STAT_MIN_VALUE);
          if (table_field->write_stat.max_value && 
              table_field->update_max(table_field->write_stat.max_value,
                                      rows == table_field->nulls))
            set_not_null_for_write_column_stat_value(table_field,
                                                     COLUMN_STAT_MAX_VALUE);
          if (table_field->count_distinct) 
            table_field->count_distinct->add();          
        }
      }
      rows++;
    }
    file->ha_rnd_end();
  }
  rc= rc == HA_ERR_END_OF_FILE ? 0 : 1;

  /* 
    Calculate values for all statistical characteristics on columns and
    and for each field f of 'table' save them in the write_stat structure
    from the Field object for f. 
  */
  if (!rc)
  {
    table->write_stat.cardinality_is_null= FALSE;
    table->write_stat.cardinality= rows;

    for (field_ptr= table->field; *field_ptr; field_ptr++)
    {
      table_field= *field_ptr;
      table_field->write_stat.nulls_ratio= (double) table_field->nulls/rows;
      table_field->write_stat.avg_length=
        (double) table_field->column_total_length / (rows-table_field->nulls);
      if (table_field->count_distinct)
      {
        table_field->write_stat.avg_frequency= 
          (double) (rows-table_field->nulls) /
          table_field->count_distinct->get_value(); 
        set_not_null_for_write_column_stat_value(table_field,
                                                 COLUMN_STAT_AVG_FREQUENCY);
        delete table_field->count_distinct;
        table_field->count_distinct= NULL;
      }
      
      set_not_null_for_write_column_stat_value(table_field,
                                               COLUMN_STAT_NULLS_RATIO);
      set_not_null_for_write_column_stat_value(table_field,
                                               COLUMN_STAT_AVG_LENGTH);
    }
  }

  if (!rc)
  {
    uint keys= table->s->keys ;

    /* Collect statistics for indexes */
    for (uint i= 0; i < keys; i++)
    {
      if ((rc= collect_statistics_for_index(table, i)))
        break;
    }
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
  statistical characteritics exist they are updated with the new statistical
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
  int rc= 0;
  TABLE *stat_table;
  uint keys= table->s->keys;

  DBUG_ENTER("update_statistics_for_table");

  init_table_list_for_stat_tables(tables, TRUE);
  init_mdl_requests(tables);

  if (unlock_tables_n_open_system_tables_for_write(thd,
                                                   tables,
                                                   &open_tables_backup))
    DBUG_RETURN(1);
   
  /* Update the statistical table table_stat */
  stat_table= tables[TABLE_STAT].table;
  Table_stat table_stat(stat_table, table);
  restore_record(stat_table, s->default_values);
  table_stat.set_key_fields();
  err= table_stat.update_stat();
  if (err)
    rc= 1;

  /* Update the statistical table colum_stat */
  stat_table= tables[COLUMN_STAT].table;
  Column_stat column_stat(stat_table, table);
  for (Field **field_ptr= table->field; *field_ptr; field_ptr++)
  {
    Field *table_field= *field_ptr;
    restore_record(stat_table, s->default_values);
    column_stat.set_key_fields(table_field);
    err= column_stat.update_stat();
    if (err & !rc)
      rc= 1;
  }

  /* Update the statistical table index_stat */
  stat_table= tables[INDEX_STAT].table;
  Index_stat index_stat(stat_table, table);
  KEY *key_info, *key_info_end;

  for (key_info= table->key_info, key_info_end= table->key_info+keys;
       key_info < key_info_end; key_info++)
  {
    uint key_parts= key_info->key_parts;
    for (i= 0; i < key_parts; i++)
    {
      restore_record(stat_table, s->default_values);
      index_stat.set_key_fields(key_info, i+1);
      err= index_stat.update_stat();
      if (err & !rc)
        rc= 1;
    }
  }

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

  @details
  For each statistical table the function looks for the rows from this
  table that contain statistical data on 'table'. If such rows is found
  the data from statistical columns of it is read into the appropriate
  fields of internal structures for 'table'. Later at the query processing
  this data are supposed to be used by the optimizer. 
  The function is called in function open_tables.

  @retval
  0         If data has been succesfully read from all statistical tables  
  @retval
  1         Otherwise

  @note
  The function first calls the function open_system_tables_for_read to
  be able to read info from the statistical tables. On success the data is
  read from one table after another after which the statistical tables are
  closed. Objects of the helper classes Table_stat, Column_stat and Index_stat
  are employed to read statistical data from the statistical tables. 
  TODO. Consider a variant when statistical tables are opened and closed
  only once for all tables, not for every table of the query as it's done
  now.        
*/

int read_statistics_for_table(THD *thd, TABLE *table)
{
  uint i;
  TABLE *stat_table;
  Field *table_field;
  Field **field_ptr;
  KEY *key_info, *key_info_end;
  TABLE_LIST tables[STATISTICS_TABLES];
  Open_tables_backup open_tables_backup;

  DBUG_ENTER("read_statistics_for_table");

  init_table_list_for_stat_tables(tables, FALSE);  
  init_mdl_requests(tables);
  
  if (open_system_tables_for_read(thd, tables, &open_tables_backup))
    DBUG_RETURN(1);

  create_min_max_stistical_fields(table, FALSE);
 
  /* Read statistics from the statistical table index_stat */
  stat_table= tables[TABLE_STAT].table;
  Table_stat table_stat(stat_table, table);
  table_stat.set_key_fields();
  table_stat.get_stat_values();
   
  /* Read statistics from the statistical table column_stat */
  stat_table= tables[COLUMN_STAT].table;
  Column_stat column_stat(stat_table, table);
  for (field_ptr= table->field; *field_ptr; field_ptr++)
  {
    table_field= *field_ptr;
    column_stat.set_key_fields(table_field);
    column_stat.get_stat_values();
  }

  /* Read statistics from the statistical table index_stat */
  stat_table= tables[INDEX_STAT].table;
  Index_stat index_stat(stat_table, table);
  for (key_info= table->key_info, key_info_end= key_info+table->s->keys;
       key_info < key_info_end; key_info++)
  {

    for (i= 0; i < key_info->key_parts; i++)
    {
      index_stat.set_key_fields(key_info, i+1);
      index_stat.get_stat_values();
    }
   
    key_part_map ext_key_part_map= key_info->ext_key_part_map;
    if (key_info->key_parts != key_info->ext_key_parts)
    {
      KEY *pk_key_info= table->key_info + table->s->primary_key;
      uint k= key_info->key_parts;
      double k_avg_frequency= key_info->read_stat.avg_frequency[k-1];
      uint pk_parts= pk_key_info->key_parts;
      ha_rows n_rows= table->read_stat.cardinality;
      for (uint j= 0; j < pk_parts; j++)
      {
        double avg_frequency;
        if (!(ext_key_part_map & 1 << j))
          continue;
        avg_frequency= pk_key_info->read_stat.avg_frequency[j];
        if (avg_frequency == 0 ||
            table->read_stat.cardinality_is_null)
          avg_frequency= 1;
        else if (avg_frequency > 1)
          avg_frequency= max(k_avg_frequency * avg_frequency / n_rows, 1);
        key_info->read_stat.avg_frequency[k++]= avg_frequency;
      }
    }
  }
      
  close_system_tables(thd, &open_tables_backup);

  DBUG_RETURN(0);
}


/**
  @brief
  Set statistics for a table that will be used by the optimizer 

  @param
  thd         The thread handle
  @param
  table       The table to set statistics for 

  @details
  Depending on the value of thd->variables.optimizer_use_stat_tables 
  the function performs the settings for the table that will control
  from where the statistical data used by the optimizer will be taken.
*/

void set_statistics_for_table(THD *thd, TABLE *table)
{
  uint use_stat_table_mode= thd->variables.optimizer_use_stat_tables;
  table->used_stat_records= 
    (use_stat_table_mode <= 1 || table->read_stat.cardinality_is_null) ?
    table->file->stats.records : table->read_stat.cardinality;
  KEY *key_info, *key_info_end;
  for (key_info= table->key_info, key_info_end= key_info+table->s->keys;
       key_info < key_info_end; key_info++)
  {
    key_info->is_statistics_from_stat_tables=
      (use_stat_table_mode > 1 &&  key_info->read_stat.avg_frequency &&
       key_info->read_stat.avg_frequency[0] > 0.5);
  }
}
