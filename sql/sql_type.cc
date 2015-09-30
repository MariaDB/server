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

#include "sql_type.h"

static Type_handler_tiny        type_handler_tiny;
static Type_handler_short       type_handler_short;
static Type_handler_long        type_handler_long;
static Type_handler_longlong    type_handler_longlong;
static Type_handler_int24       type_handler_int24;
static Type_handler_year        type_handler_year;
static Type_handler_bit         type_handler_bit;
static Type_handler_float       type_handler_float;
static Type_handler_double      type_handler_double;
static Type_handler_time        type_handler_time;
static Type_handler_date        type_handler_date;
static Type_handler_datetime    type_handler_datetime;
static Type_handler_timestamp   type_handler_timestamp;
static Type_handler_olddecimal  type_handler_olddecimal;
static Type_handler_newdecimal  type_handler_newdecimal;
static Type_handler_null        type_handler_null;
static Type_handler_string      type_handler_string;
static Type_handler_varchar     type_handler_varchar;
static Type_handler_tiny_blob   type_handler_tiny_blob;
static Type_handler_medium_blob type_handler_medium_blob;
static Type_handler_long_blob   type_handler_long_blob;
static Type_handler_blob        type_handler_blob;
static Type_handler_geometry    type_handler_geometry;


Type_handler_hybrid_field_type::Type_handler_hybrid_field_type()
  :m_type_handler(&type_handler_double)
{
}


const Type_handler *
Type_handler_hybrid_field_type::get_handler_by_result_type(Item_result type)
                                                           const
{
  switch (type) {
  case REAL_RESULT:       return &type_handler_double;
  case INT_RESULT:        return &type_handler_longlong;
  case DECIMAL_RESULT:    return &type_handler_newdecimal;
  case STRING_RESULT:     return &type_handler_string;
  case TIME_RESULT:
  case ROW_RESULT:
    DBUG_ASSERT(0);
  }
  return &type_handler_string;
}


const Type_handler *
Type_handler_hybrid_field_type::get_handler_by_field_type(enum_field_types type)
                                                          const
{
  switch (type) {
  case MYSQL_TYPE_DECIMAL:     return &type_handler_olddecimal;
  case MYSQL_TYPE_NEWDECIMAL:  return &type_handler_newdecimal;
  case MYSQL_TYPE_TINY:        return &type_handler_tiny;
  case MYSQL_TYPE_SHORT:       return &type_handler_short;
  case MYSQL_TYPE_LONG:        return &type_handler_long;
  case MYSQL_TYPE_LONGLONG:    return &type_handler_longlong;
  case MYSQL_TYPE_INT24:       return &type_handler_int24;
  case MYSQL_TYPE_YEAR:        return &type_handler_year;
  case MYSQL_TYPE_BIT:         return &type_handler_bit;
  case MYSQL_TYPE_FLOAT:       return &type_handler_float;
  case MYSQL_TYPE_DOUBLE:      return &type_handler_double;
  case MYSQL_TYPE_NULL:        return &type_handler_null;
  case MYSQL_TYPE_VARCHAR:     return &type_handler_varchar;     
  case MYSQL_TYPE_TINY_BLOB:   return &type_handler_tiny_blob;
  case MYSQL_TYPE_MEDIUM_BLOB: return &type_handler_medium_blob;
  case MYSQL_TYPE_LONG_BLOB:   return &type_handler_long_blob;
  case MYSQL_TYPE_BLOB:        return &type_handler_blob;
  case MYSQL_TYPE_VAR_STRING:  return &type_handler_varchar; // Map to VARCHAR 
  case MYSQL_TYPE_STRING:      return &type_handler_string;
  case MYSQL_TYPE_ENUM:        return &type_handler_varchar; // Map to VARCHAR
  case MYSQL_TYPE_SET:         return &type_handler_varchar; // Map to VARCHAR
  case MYSQL_TYPE_GEOMETRY:    return &type_handler_geometry;
  case MYSQL_TYPE_TIMESTAMP:   return &type_handler_timestamp;
  case MYSQL_TYPE_TIMESTAMP2:  return &type_handler_timestamp;
  case MYSQL_TYPE_DATE:        return &type_handler_date;
  case MYSQL_TYPE_TIME:        return &type_handler_time;
  case MYSQL_TYPE_TIME2:       return &type_handler_time;
  case MYSQL_TYPE_DATETIME:    return &type_handler_datetime;
  case MYSQL_TYPE_DATETIME2:   return &type_handler_datetime;
  case MYSQL_TYPE_NEWDATE:     return &type_handler_date;
  };
  DBUG_ASSERT(0);
  return &type_handler_string;
}

