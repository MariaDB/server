#ifndef SQL_TYPE_H_INCLUDED
#define SQL_TYPE_H_INCLUDED
/*
   Copyright (c) 2015  MariaDB Foundation.

 This program is free software; you can redistribute it and/or modify
 it under the terms of the GNU General Public License as published by
 the Free Software Foundation; version 2 of the License.

 This program is distributed in the hope that it will be useful,
 but WITHOUT ANY WARRANTY; without even the implied warranty of
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 GNU General Public License for more details.

 You should have received a copy of the GNU General Public License
 along with this program; if not, write to the Free Software
 Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA */

#ifdef USE_PRAGMA_INTERFACE
#pragma interface			/* gcc class implementation */
#endif


#include "mysqld.h"

class Type_handler
{
protected:
  const Type_handler *string_type_handler(uint max_octet_length) const;
public:
  virtual enum_field_types field_type() const= 0;
  virtual Item_result result_type() const= 0;
  virtual Item_result cmp_type() const= 0;
  virtual const Type_handler*
  type_handler_adjusted_to_max_octet_length(uint max_octet_length,
                                            CHARSET_INFO *cs) const
  { return this; }
  virtual ~Type_handler() {}
};


/*** Abstract classes for every XXX_RESULT */

class Type_handler_real_result: public Type_handler
{
public:
  Item_result result_type() const { return REAL_RESULT; }
  Item_result cmp_type() const { return REAL_RESULT; }
  virtual ~Type_handler_real_result() {}
};


class Type_handler_decimal_result: public Type_handler
{
public:
  Item_result result_type() const { return DECIMAL_RESULT; }
  Item_result cmp_type() const { return DECIMAL_RESULT; }
  virtual ~Type_handler_decimal_result() {};
};


class Type_handler_int_result: public Type_handler
{
public:
  Item_result result_type() const { return INT_RESULT; }
  Item_result cmp_type() const { return INT_RESULT; }
  virtual ~Type_handler_int_result() {}
};


class Type_handler_temporal_result: public Type_handler
{
public:
  Item_result result_type() const { return STRING_RESULT; }
  Item_result cmp_type() const { return TIME_RESULT; }
  virtual ~Type_handler_temporal_result() {}
};


class Type_handler_string_result: public Type_handler
{
public:
  Item_result result_type() const { return STRING_RESULT; }
  Item_result cmp_type() const { return STRING_RESULT; }
  virtual ~Type_handler_string_result() {}
  const Type_handler *
  type_handler_adjusted_to_max_octet_length(uint max_octet_length,
                                            CHARSET_INFO *cs) const;
};


/***
  Instantiable classes for every MYSQL_TYPE_XXX

  There are no Type_handler_xxx for the following types:
  - MYSQL_TYPE_VAR_STRING (old VARCHAR) - mapped to MYSQL_TYPE_VARSTRING
  - MYSQL_TYPE_ENUM                     - mapped to MYSQL_TYPE_VARSTRING
  - MYSQL_TYPE_SET:                     - mapped to MYSQL_TYPE_VARSTRING

  because the functionality that currently uses Type_handler
  (e.g. hybrid type functions) does not need to distinguish between
  these types and VARCHAR.
  For example:
    CREATE TABLE t2 AS SELECT COALESCE(enum_column) FROM t1;
  creates a VARCHAR column.

  There most likely be Type_handler_enum and Type_handler_set later,
  when the Type_handler infrastructure gets used in more pieces of the code.
*/


class Type_handler_tiny: public Type_handler_int_result
{
public:
  virtual ~Type_handler_tiny() {}
  enum_field_types field_type() const { return MYSQL_TYPE_TINY; }
};


class Type_handler_short: public Type_handler_int_result
{
public:
  virtual ~Type_handler_short() {}
  enum_field_types field_type() const { return MYSQL_TYPE_SHORT; }
};


class Type_handler_long: public Type_handler_int_result
{
public:
  virtual ~Type_handler_long() {}
  enum_field_types field_type() const { return MYSQL_TYPE_LONG; }
};


class Type_handler_longlong: public Type_handler_int_result
{
public:
  virtual ~Type_handler_longlong() {}
  enum_field_types field_type() const { return MYSQL_TYPE_LONGLONG; }
};


class Type_handler_int24: public Type_handler_int_result
{
public:
  virtual ~Type_handler_int24() {}
  enum_field_types field_type() const { return MYSQL_TYPE_INT24; }
};


class Type_handler_year: public Type_handler_int_result
{
public:
  virtual ~Type_handler_year() {}
  enum_field_types field_type() const { return MYSQL_TYPE_YEAR; }
};


class Type_handler_bit: public Type_handler_int_result
{
public:
  virtual ~Type_handler_bit() {}
  enum_field_types field_type() const { return MYSQL_TYPE_BIT; }
};


class Type_handler_float: public Type_handler_real_result
{
public:
  virtual ~Type_handler_float() {}
  enum_field_types field_type() const { return MYSQL_TYPE_FLOAT; }
};


class Type_handler_double: public Type_handler_real_result
{
public:
  virtual ~Type_handler_double() {}
  enum_field_types field_type() const { return MYSQL_TYPE_DOUBLE; }
};


class Type_handler_time: public Type_handler_temporal_result
{
public:
  virtual ~Type_handler_time() {}
  enum_field_types field_type() const { return MYSQL_TYPE_TIME; }
};


class Type_handler_date: public Type_handler_temporal_result
{
public:
  virtual ~Type_handler_date() {}
  enum_field_types field_type() const { return MYSQL_TYPE_DATE; }
};


class Type_handler_datetime: public Type_handler_temporal_result
{
public:
  virtual ~Type_handler_datetime() {}
  enum_field_types field_type() const { return MYSQL_TYPE_DATETIME; }
};


class Type_handler_timestamp: public Type_handler_temporal_result
{
public:
  virtual ~Type_handler_timestamp() {}
  enum_field_types field_type() const { return MYSQL_TYPE_TIMESTAMP; }
};


class Type_handler_olddecimal: public Type_handler_decimal_result
{
public:
  virtual ~Type_handler_olddecimal() {}
  enum_field_types field_type() const { return MYSQL_TYPE_DECIMAL; }
};


class Type_handler_newdecimal: public Type_handler_decimal_result
{
public:
  virtual ~Type_handler_newdecimal() {}
  enum_field_types field_type() const { return MYSQL_TYPE_NEWDECIMAL; }
};


class Type_handler_null: public Type_handler_string_result
{
public:
  virtual ~Type_handler_null() {}
  enum_field_types field_type() const { return MYSQL_TYPE_NULL; }
};


class Type_handler_string: public Type_handler_string_result
{
public:
  virtual ~Type_handler_string() {}
  enum_field_types field_type() const { return MYSQL_TYPE_STRING; }
};


class Type_handler_varchar: public Type_handler_string_result
{
public:
  virtual ~Type_handler_varchar() {}
  enum_field_types field_type() const { return MYSQL_TYPE_VARCHAR; }
};


class Type_handler_tiny_blob: public Type_handler_string_result
{
public:
  virtual ~Type_handler_tiny_blob() {}
  enum_field_types field_type() const { return MYSQL_TYPE_TINY_BLOB; }
};


class Type_handler_medium_blob: public Type_handler_string_result
{
public:
  virtual ~Type_handler_medium_blob() {}
  enum_field_types field_type() const { return MYSQL_TYPE_MEDIUM_BLOB; }
};


class Type_handler_long_blob: public Type_handler_string_result
{
public:
  virtual ~Type_handler_long_blob() {}
  enum_field_types field_type() const { return MYSQL_TYPE_LONG_BLOB; }
};


class Type_handler_blob: public Type_handler_string_result
{
public:
  virtual ~Type_handler_blob() {}
  enum_field_types field_type() const { return MYSQL_TYPE_BLOB; }
};


class Type_handler_geometry: public Type_handler_string_result
{
public:
  virtual ~Type_handler_geometry() {}
  enum_field_types field_type() const { return MYSQL_TYPE_GEOMETRY; }
};


/**
  A handler for hybrid type functions, e.g.
  COALESCE(), IF(), IFNULL(), NULLIF(), CASE,
  numeric operators,
  UNIX_TIMESTAMP(), TIME_TO_SEC().

  Makes sure that field_type(), cmp_type() and result_type()
  are always in sync to each other for hybrid functions.
*/
class Type_handler_hybrid_field_type: public Type_handler
{
  const Type_handler *m_type_handler;
  const Type_handler *get_handler_by_result_type(Item_result type) const;
  const Type_handler *get_handler_by_field_type(enum_field_types type) const;
public:
  Type_handler_hybrid_field_type();
  Type_handler_hybrid_field_type(enum_field_types type)
    :m_type_handler(get_handler_by_field_type(type))
  { }
  Type_handler_hybrid_field_type(const Type_handler_hybrid_field_type *other)
    :m_type_handler(other->m_type_handler)
  { }
  enum_field_types field_type() const { return m_type_handler->field_type(); }
  Item_result result_type() const { return m_type_handler->result_type(); }
  Item_result cmp_type() const { return m_type_handler->cmp_type(); }
  const Type_handler *set_handler_by_result_type(Item_result type)
  {
    return (m_type_handler= get_handler_by_result_type(type));
  }
  const Type_handler *set_handler_by_result_type(Item_result type,
                                                 uint max_octet_length,
                                                 CHARSET_INFO *cs)
  {
    m_type_handler= get_handler_by_result_type(type);
    return m_type_handler=
      m_type_handler->type_handler_adjusted_to_max_octet_length(max_octet_length,
                                                                cs);
  }
  const Type_handler *set_handler_by_field_type(enum_field_types type)
  {
    return (m_type_handler= get_handler_by_field_type(type));
  }
  const Type_handler *
  type_handler_adjusted_to_max_octet_length(uint max_octet_length,
                                            CHARSET_INFO *cs) const
  {
    return
      m_type_handler->type_handler_adjusted_to_max_octet_length(max_octet_length,
                                                                cs);
  }
};

#endif /* SQL_TYPE_H_INCLUDED */
