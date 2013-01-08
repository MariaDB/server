/* Copyright 2006-2008 MySQL AB, 2008 Sun Microsystems, Inc.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA */

#ifndef SQL_STATISTICS_H
#define SQL_STATISTICS_H

/* 
  These enumeration types comprise the dictionary of three
  statistical tables table_stat, column_stat and index_stat
  as they defined in ../scripts/mysql_system_tables.sql.

  It would be nice if the declarations of these types were
  generated automatically by the table definitions.   
*/

typedef
enum enum_use_stat_tables_mode
{
  NEVER,
  COMPLEMENTARY,
  PEFERABLY,
} Use_stat_tables_mode;

enum enum_stat_tables
{
  TABLE_STAT,
  COLUMN_STAT,
  INDEX_STAT,
};

enum enum_table_stat_col
{
  TABLE_STAT_DB_NAME,
  TABLE_STAT_TABLE_NAME,
  TABLE_STAT_CARDINALITY
};

enum enum_column_stat_col
{
  COLUMN_STAT_DB_NAME,
  COLUMN_STAT_TABLE_NAME,
  COLUMN_STAT_COLUMN_NAME,
  COLUMN_STAT_MIN_VALUE,
  COLUMN_STAT_MAX_VALUE,
  COLUMN_STAT_NULLS_RATIO,
  COLUMN_STAT_AVG_LENGTH,
  COLUMN_STAT_AVG_FREQUENCY
};

enum enum_index_stat_col
{
  INDEX_STAT_DB_NAME,
  INDEX_STAT_TABLE_NAME,
  INDEX_STAT_INDEX_NAME,
  INDEX_STAT_PREFIX_ARITY,
  INDEX_STAT_AVG_FREQUENCY
};

inline
Use_stat_tables_mode get_use_stat_tables_mode(THD *thd)
{ 
  return (Use_stat_tables_mode) (thd->variables.use_stat_tables);
}

int read_statistics_for_tables_if_needed(THD *thd, TABLE_LIST *tables);
int collect_statistics_for_table(THD *thd, TABLE *table);
int alloc_statistics_for_table_share(THD* thd, TABLE_SHARE *share,
                                     bool is_safe);
int alloc_statistics_for_table(THD *thd, TABLE *table);
int update_statistics_for_table(THD *thd, TABLE *table);
int delete_statistics_for_table(THD *thd, LEX_STRING *db, LEX_STRING *tab);
int delete_statistics_for_column(THD *thd, TABLE *tab, Field *col);
int delete_statistics_for_index(THD *thd, TABLE *tab, KEY *key_info,
                                bool ext_prefixes_only);
int rename_table_in_stat_tables(THD *thd, LEX_STRING *db, LEX_STRING *tab,
                                LEX_STRING *new_db, LEX_STRING *new_tab);
int rename_column_in_stat_tables(THD *thd, TABLE *tab, Field *col,
                                  const char *new_name);
void set_statistics_for_table(THD *thd, TABLE *table);

class Columns_statistics;
class Index_statistics;


/* Statistical data on a table */

class Table_statistics
{

public:
  my_bool cardinality_is_null;      /* TRUE if the cardinality is unknown */
  ha_rows cardinality;              /* Number of rows in the table        */
  uchar *min_max_record_buffers;    /* Record buffers for min/max values  */
  Column_statistics *column_stats;  /* Array of statistical data for columns */
  Index_statistics *index_stats;    /* Array of statistical data for indexes */
  ulong *idx_avg_frequency;   /* Array of records per key for index prefixes */                                       

};


/* Statistical data on a column */

class Column_statistics
{

private:
  static const uint Scale_factor_nulls_ratio= 100000;
  static const uint Scale_factor_avg_length= 100000;
  static const uint Scale_factor_avg_frequency= 100000;

public:
  /* 
    Bitmap indicating  what statistical characteristics
    are available for the column
  */
  uint32 column_stat_nulls;

  /* Minimum value for the column */
  Field *min_value; 
  /* Maximum value for the column */   
  Field *max_value;

private:

  /* 
    The ratio Z/N multiplied by the scale factor Scale_factor_nulls_ratio,
    where 
      N is the total number of rows,
      Z is the number of nulls in the column
  */
  ulong nulls_ratio;
 
  /*
    Average number of bytes occupied by the representation of a
    value of the column in memory buffers such as join buffer
    multiplied by the scale factor Scale_factor_avg_length.
    CHAR values are stripped of trailing spaces.
    Flexible values are stripped of their length prefixes.
  */
  ulong avg_length;

  /*
    The ratio N/D multiplied by the scale factor Scale_factor_avg_frequency,
    where
       N is the number of rows with not null value in the column,
       D the number of distinct values among them
  */
  ulong avg_frequency;

public:

  void set_all_nulls()
  {
    column_stat_nulls= 
      ((1 << (COLUMN_STAT_AVG_FREQUENCY-COLUMN_STAT_COLUMN_NAME))-1) <<
      (COLUMN_STAT_COLUMN_NAME+1);
  }

  void set_not_null(uint stat_field_no)
  {
    column_stat_nulls&= ~(1 << stat_field_no);
  }

  bool is_null(uint stat_field_no)
  {
    return test(column_stat_nulls & (1 << stat_field_no));
  }

  double get_nulls_ratio()
  {
    return (double) nulls_ratio /  Scale_factor_nulls_ratio;
  }

  double get_avg_length()
  {
    return (double) avg_length / Scale_factor_avg_length;
  }

  double get_avg_frequency()
  {
    return (double) avg_frequency / Scale_factor_avg_frequency;
  }

  void set_nulls_ratio (double val)
  {
    nulls_ratio= (ulong) (val * Scale_factor_nulls_ratio);
  }

  void set_avg_length (double val)
  {
    avg_length= (ulong) (val * Scale_factor_avg_length);
  }

  void set_avg_frequency (double val)
  {
    avg_frequency= (ulong) (val * Scale_factor_avg_frequency);
  }

};


/* Statistical data on an index prefixes */

class Index_statistics
{

private:
  static const uint Scale_factor_avg_frequency= 100000;
  /*
    The k-th element of this array contains the ratio N/D
    multiplied by the scale factor Scale_factor_avg_frequency, 
    where N is the number of index entries without nulls 
    in the first k components, and D is the number of distinct
    k-component prefixes among them 
  */
  ulong *avg_frequency;

public:

  void init_avg_frequency(ulong *ptr) { avg_frequency= ptr; }

  bool avg_frequency_is_inited() { return avg_frequency != NULL; }

  double get_avg_frequency(uint i)
  {
    return (double) avg_frequency[i] / Scale_factor_avg_frequency;
  }

  void set_avg_frequency(uint i, double val)
  {
    avg_frequency[i]= (ulong) (val * Scale_factor_avg_frequency);
  }

};

#endif /* SQL_STATISTICS_H */
