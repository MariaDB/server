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
#include "sql_string.h"             // strlen, MY_CS_CHARACTER_SET_NAME_SIZE
#include "lex_string.h"             // LEX_CSTRING
#include "mysql_com.h"              // enum_field_types
#include "my_time.h"                // TIME_SECOND_PART_DIGITS
#include "sql_type.h"               // Type_handler_xxx

struct TABLE_LIST;
struct TABLE;
typedef class Item COND;

#ifdef MYSQL_CLIENT
#error MYSQL_CLIENT must not be defined
#endif // MYSQL_CLIENT


bool schema_table_store_record(THD *thd, TABLE *table);
COND *make_cond_for_info_schema(THD *thd, COND *cond, TABLE_LIST *table);


enum enum_show_open_table
{
  SKIP_OPEN_TABLE= 0U,              // do not open table
  OPEN_FRM_ONLY=   1U,              // open FRM file only
  OPEN_FULL_TABLE= 2U               // open FRM,MYD, MYI files
};


namespace Show {
class Type
{
  /**
     This denotes data type for the column. For the most part, there seems to
     be one entry in the enum for each SQL data type, although there seem to
     be a number of additional entries in the enum.
  */
  const Type_handler *m_type_handler;
  /**
     For string-type columns, this is the maximum number of
     characters. Otherwise, it is the 'display-length' for the column.
  */
  uint m_char_length;
  uint m_unsigned_flag;
  const Typelib *m_typelib;
public:
  Type(const Type_handler *th, uint length, uint unsigned_flag,
       const Typelib *typelib= NULL)
   :m_type_handler(th), m_char_length(length), m_unsigned_flag(unsigned_flag),
    m_typelib(typelib)
  { }
  const Type_handler *type_handler() const { return m_type_handler; }
  uint char_length()      const { return m_char_length; }
  decimal_digits_t decimal_precision() const
  { return (decimal_digits_t) ((m_char_length / 100) % 100); }
  decimal_digits_t decimal_scale() const
  { return (decimal_digits_t) (m_char_length % 10); }
  uint fsp() const
  {
    DBUG_ASSERT(m_char_length <= TIME_SECOND_PART_DIGITS);
    return m_char_length;
  }
  uint unsigned_flag()    const { return m_unsigned_flag; }
  const Typelib *typelib() const { return m_typelib; }
};
} // namespace Show



class ST_FIELD_INFO: public Show::Type
{
protected:
  LEX_CSTRING m_name;                 // I_S column name
  enum_nullability m_nullability;     // NULLABLE or NOT NULL
  LEX_CSTRING m_old_name;             // SHOW column name
  enum_show_open_table m_open_method;
public:
  ST_FIELD_INFO(const LEX_CSTRING &name, const Type &type,
                enum_nullability nullability,
                LEX_CSTRING &old_name,
                enum_show_open_table open_method)
   :Type(type), m_name(name),
    m_nullability(nullability),
    m_old_name(old_name),
    m_open_method(open_method)
  { }
  ST_FIELD_INFO(const char *name, const Type &type,
                enum_nullability nullability,
                const char *old_name,
                enum_show_open_table open_method)
   :Type(type),
    m_nullability(nullability),
    m_open_method(open_method)
  {
    m_name.str= name;
    m_name.length= safe_strlen(name);
    m_old_name.str= old_name;
    m_old_name.length= safe_strlen(old_name);
  }
  const LEX_CSTRING &name() const { return m_name; }
  bool nullable() const { return m_nullability == NULLABLE; }
  const LEX_CSTRING &old_name() const { return m_old_name; }
  enum_show_open_table open_method() const { return  m_open_method; }
  bool end_marker() const { return m_name.str == NULL; }
};


namespace Show
{


class Enum: public Type
{
public:
  Enum(const Typelib *typelib) :Type(&type_handler_enum, 0, false, typelib) { }
};


class Blob: public Type
{
public:
  Blob(uint length) :Type(&type_handler_blob, length, false) { }
};


class Varchar: public Type
{
public:
  Varchar(uint length) :Type(&type_handler_varchar, length, false)
  {
    DBUG_ASSERT(length * 3 <= MAX_FIELD_VARCHARLENGTH);
  }
};


class Longtext: public Type
{
public:
  Longtext(uint length) :Type(&type_handler_varchar, length, false) { }
};


class Yes_or_empty: public Varchar
{
public:
  Yes_or_empty(): Varchar(3) { }
  static LEX_CSTRING value(bool val)
  {
    return val ? Lex_cstring(STRING_WITH_LEN("Yes")) :
                 Lex_cstring();
  }
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
  CSName(): Varchar(MY_CS_CHARACTER_SET_NAME_SIZE) { }
};


class CLName: public Varchar
{
public:
  CLName(): Varchar(MY_CS_COLLATION_NAME_SIZE) { }
};


class SQLMode: public Varchar
{
public:
  SQLMode(): Varchar(32*256) { }
};


class Datetime: public Type
{
public:
  Datetime(uint dec) :Type(&type_handler_datetime2, dec, false) { }
};


class Decimal: public Type
{
public:
  Decimal(uint length) :Type(&type_handler_newdecimal, length, false) { }
};


class ULonglong: public Type
{
public:
  ULonglong(uint length) :Type(&type_handler_ulonglong, length, true) { }
  ULonglong() :ULonglong(MY_INT64_NUM_DECIMAL_DIGITS) { }
};


class ULong: public Type
{
public:
  ULong(uint length) :Type(&type_handler_ulong, length, true) { }
  ULong() :ULong(MY_INT32_NUM_DECIMAL_DIGITS) { }
};


class SLonglong: public Type
{
public:
  SLonglong(uint length) :Type(&type_handler_slonglong, length, false) { }
  SLonglong() :SLonglong(MY_INT64_NUM_DECIMAL_DIGITS) { }
};


class SLong: public Type
{
public:
  SLong(uint length) :Type(&type_handler_slong, length, false) { }
  SLong() :SLong(MY_INT32_NUM_DECIMAL_DIGITS) { }
};


class SShort: public Type
{
public:
  SShort(uint length) :Type(&type_handler_sshort, length, false) { }
};


class STiny: public Type
{
public:
  STiny(uint length) :Type(&type_handler_stiny, length, false) { }
};


class Double: public Type
{
public:
  Double(uint length) :Type(&type_handler_double, length, false) { }
};


class Float: public Type
{
public:
  Float(uint length) :Type(&type_handler_float, length, false) { }
};



class Column: public ST_FIELD_INFO
{
public:
  Column(const char *name, const Type &type,
         enum_nullability nullability,
         const char *old_name,
         enum_show_open_table open_method= SKIP_OPEN_TABLE)
   :ST_FIELD_INFO(name, type, nullability,
                  old_name, open_method)
  { }
  Column(const char *name, const Type &type,
         enum_nullability nullability,
         enum_show_open_table open_method= SKIP_OPEN_TABLE)
   :ST_FIELD_INFO(name, type, nullability,
                  NullS, open_method)
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
