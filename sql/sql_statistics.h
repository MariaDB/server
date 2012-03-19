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

#endif /* SQL_STATISTICS_H */
