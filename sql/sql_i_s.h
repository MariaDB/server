#ifndef SQL_I_S_INCLUDED
#define SQL_I_S_INCLUDED
/* Copyright (c) 2000, 2017, Oracle and/or its affiliates.
   Copyright (c) 2009, 2019, MariaDB

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1335  USA */

#include "sql_const.h"              // MAX_FIELD_VARCHARLENGTH
#include "sql_basic_types.h"        // enum_nullability
#include "sql_string.h"             // strlen, MY_CS_NAME_SIZE
#include "lex_string.h"             // LEX_CSTRING
#include "mysql_com.h"              // enum_field_types
#include "my_time.h"                // TIME_SECOND_PART_DIGITS

struct TABLE_LIST;
struct TABLE;
typedef class Item COND;

#ifdef MYSQL_CLIENT
#error MYSQL_CLIENT must not be defined
#endif // MYSQL_CLIENT


bool schema_table_store_record(THD *thd, TABLE *table);
COND *make_cond_for_info_schema(THD *thd, COND *cond, TABLE_LIST *table);


#define MY_I_S_MAYBE_NULL 1U
#define MY_I_S_UNSIGNED   2U


enum enum_show_open_table
{
  SKIP_OPEN_TABLE= 0U,              // do not open table
  OPEN_FRM_ONLY=   1U,              // open FRM file only
  OPEN_FULL_TABLE= 2U               // open FRM,MYD, MYI files
};


struct ST_FIELD_INFO
{
  /** 
      This is used as column name. 
  */
  const char* field_name;
  /**
     For string-type columns, this is the maximum number of
     characters. Otherwise, it is the 'display-length' for the column.
  */
  uint field_length;
  /**
     This denotes data type for the column. For the most part, there seems to
     be one entry in the enum for each SQL data type, although there seem to
     be a number of additional entries in the enum.
  */
  enum enum_field_types field_type;
  int value;
  /**
     This is used to set column attributes. By default, columns are @c NOT
     @c NULL and @c SIGNED, and you can deviate from the default
     by setting the appopriate flags. You can use either one of the flags
     @c MY_I_S_MAYBE_NULL and @cMY_I_S_UNSIGNED or
     combine them using the bitwise or operator @c |. Both flags are
     defined in table.h.
   */
  uint field_flags;        // Field atributes(maybe_null, signed, unsigned etc.)
  const char* old_name;
  /**
     This should be one of @c SKIP_OPEN_TABLE,
     @c OPEN_FRM_ONLY or @c OPEN_FULL_TABLE.
  */
  uint open_method;

  LEX_CSTRING get_name() const
  {
    return LEX_CSTRING({field_name, strlen(field_name)});
  }
  LEX_CSTRING get_old_name() const
  {
    return LEX_CSTRING({old_name, strlen(old_name)});
   }
  bool unsigned_flag() const { return field_flags & MY_I_S_UNSIGNED; }
  uint fsp() const
  {
    DBUG_ASSERT(field_length <= TIME_SECOND_PART_DIGITS);
    return field_length;
  }
};


namespace Show
{

class Type
{
  enum enum_field_types m_type;
  uint m_char_length;
  uint m_unsigned_flag;
public:
  Type(enum_field_types type, uint length, uint unsigned_flag)
   :m_type(type), m_char_length(length), m_unsigned_flag(unsigned_flag)
  { }
  enum_field_types type() const { return m_type; }
  uint char_length()      const { return m_char_length; }
  uint unsigned_flag()    const { return m_unsigned_flag; }
};


class Blob: public Type
{
public:
  Blob(uint length) :Type(MYSQL_TYPE_BLOB, length, false) { }
};


class Varchar: public Type
{
public:
  Varchar(uint length) :Type(MYSQL_TYPE_STRING, length, false)
  {
    DBUG_ASSERT(length * 3 <= MAX_FIELD_VARCHARLENGTH);
  }
};


class Longtext: public Type
{
public:
  Longtext(uint length) :Type(MYSQL_TYPE_STRING, length, false) { }
};


class Yesno: public Varchar
{
public:
  Yesno(): Varchar(3) { }
};


class Catalog: public Varchar
{
public:
  Catalog(): Varchar(FN_REFLEN) { }
};


class Name: public Varchar
{
public:
  Name(): Varchar(NAME_CHAR_LEN) { }
};


class Definer: public Varchar
{
public:
  Definer(): Varchar(DEFINER_CHAR_LENGTH) { }
};


class Userhost: public Varchar
{
public:
  Userhost(): Varchar(USERNAME_CHAR_LENGTH + HOSTNAME_LENGTH + 2) { }
};


class CSName: public Varchar
{
public:
  CSName(): Varchar(MY_CS_NAME_SIZE) { }
};


class SQLMode: public Varchar
{
public:
  SQLMode(): Varchar(32*256) { }
};


class Datetime: public Type
{
public:
  Datetime(uint dec) :Type(MYSQL_TYPE_DATETIME, dec, false) { }
};


class Decimal: public Type
{
public:
  Decimal(uint length) :Type(MYSQL_TYPE_DECIMAL, length, false) { }
};


class ULonglong: public Type
{
public:
  ULonglong(uint length) :Type(MYSQL_TYPE_LONGLONG, length, true) { }
  ULonglong() :ULonglong(MY_INT64_NUM_DECIMAL_DIGITS) { }
};


class ULong: public Type
{
public:
  ULong(uint length) :Type(MYSQL_TYPE_LONG, length, true) { }
  ULong() :ULong(MY_INT32_NUM_DECIMAL_DIGITS) { }
};


class SLonglong: public Type
{
public:
  SLonglong(uint length) :Type(MYSQL_TYPE_LONGLONG, length, false) { }
  SLonglong() :SLonglong(MY_INT64_NUM_DECIMAL_DIGITS) { }
};


class SLong: public Type
{
public:
  SLong(uint length) :Type(MYSQL_TYPE_LONG, length, false) { }
  SLong() :SLong(MY_INT32_NUM_DECIMAL_DIGITS) { }
};


class SShort: public Type
{
public:
  SShort(uint length) :Type(MYSQL_TYPE_SHORT, length, false) { }
};


class STiny: public Type
{
public:
  STiny(uint length) :Type(MYSQL_TYPE_TINY, length, false) { }
};


class Double: public Type
{
public:
  Double(uint length) :Type(MYSQL_TYPE_DOUBLE, length, false) { }
};



class Column: public ST_FIELD_INFO
{
public:
  Column(const char *name, const Type &type, enum_nullability nullability,
         const char *old_name,
         enum_show_open_table open_method= SKIP_OPEN_TABLE)
  {
    ST_FIELD_INFO::field_name= name;
    ST_FIELD_INFO::field_length= type.char_length();
    ST_FIELD_INFO::field_type= type.type();
    ST_FIELD_INFO::value= 0;
    ST_FIELD_INFO::field_flags=
      (type.unsigned_flag() ? MY_I_S_UNSIGNED : 0) |
      (nullability == NULLABLE ? MY_I_S_MAYBE_NULL : 0);
    ST_FIELD_INFO::old_name= old_name;
    ST_FIELD_INFO::open_method= open_method;
  }
  Column(const char *name, const Type &type, enum_nullability nullability,
         enum_show_open_table open_method= SKIP_OPEN_TABLE)
   :Column(name, type, nullability, NullS, open_method)
  { }
};


// End marker
class CEnd: public Column
{
public:
  CEnd() :Column(NullS, Varchar(0), NOT_NULL, NullS, SKIP_OPEN_TABLE) { }
};


} // namespace Show


struct TABLE_LIST;
typedef class Item COND;

typedef struct st_schema_table
{
  const char *table_name;
  ST_FIELD_INFO *fields_info;
  /* for FLUSH table_name */
  int (*reset_table) ();
  /* Fill table with data */
  int (*fill_table) (THD *thd, TABLE_LIST *tables, COND *cond);
  /* Handle fileds for old SHOW */
  int (*old_format) (THD *thd, struct st_schema_table *schema_table);
  int (*process_table) (THD *thd, TABLE_LIST *tables, TABLE *table,
                        bool res, const LEX_CSTRING *db_name,
                        const LEX_CSTRING *table_name);
  int idx_field1, idx_field2; 
  bool hidden;
  uint i_s_requested_object;  /* the object we need to open(TABLE | VIEW) */
} ST_SCHEMA_TABLE;


#endif // SQL_I_S_INCLUDED
