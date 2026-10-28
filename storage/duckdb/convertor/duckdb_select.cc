/*
  Copyright (c) 2026, MariaDB Foundation.
  Copyright (c) 2026, Roman Nozdrin <drrtuy@gmail.com>
  Copyright (c) 2026, Leonid Fedorov.

  This program is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation; version 2 of the License.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program; if not, write to the Free Software
  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1335 USA
*/

#include "duckdb_select.h"

#include <algorithm>
#include <cctype>
#include <exception>

#include <my_global.h>
#include "my_time.h"
#include "sql_class.h"
#include "sql_time.h"
#include "tztime.h"

#include "ddl_convertor.h"

static void store_integer_field(Field *field, duckdb::Value &value)
{
  const bool is_unsigned= field->is_unsigned();
  try
  {
    if (is_unsigned)
    {
      uint64_t v= value.GetValue<uint64_t>();
      field->store((longlong) v, true);
    }
    else
    {
      int64_t v= value.GetValue<int64_t>();
      field->store(v, false);
    }
    return;
  }
  catch (const duckdb::Exception &)
  {
  }
  catch (const std::exception &)
  {
  }
  auto str= value.ToString();
  field->store(str.c_str(), str.size(), system_charset_info);
}

/**
  Store a temporal value, used for temporal type field like DATE, DATETIME,
  TIMESTAMP, TIME.
  @param field            the field to store value
  @param ltime            the time value to be stored in field
*/
static void store_field_temporal_value(Field *field, MYSQL_TIME *ltime)
{
  field->store_time(ltime);
}

/**
  Convert a DuckDB value to a string for MariaDB string-like fields.
*/
static duckdb::string duckdb_value_as_string(const duckdb::Value &value)
{
  if (value.type().InternalType() == duckdb::PhysicalType::VARCHAR)
    return value.GetValueUnsafe<duckdb::string>();
  return value.ToString();
}

/**
  Store a duckdb value in a field of mysql format.
  @param field            the field to store value
  @param value            the value to be stored
  @param thd              the thread handle
*/
bool store_duckdb_field_in_mysql_format(Field *field, duckdb::Value &value,
                                        THD *thd)
{
  try
  {
  if (value.IsNull())
  {
    field->set_default();
    if (field->real_maybe_null())
      field->set_null();
  }
  else
  {
    field->set_notnull();
    switch (field->type())
    {
    case MYSQL_TYPE_TINY_BLOB:
    case MYSQL_TYPE_MEDIUM_BLOB:
    case MYSQL_TYPE_LONG_BLOB:
    case MYSQL_TYPE_BLOB:
    case MYSQL_TYPE_GEOMETRY:
    case MYSQL_TYPE_BIT: {
      auto str= duckdb_value_as_string(value);
      auto varchar= str.c_str();
      auto varchar_len= str.size();
      field->store(varchar, varchar_len, &my_charset_bin);
      break;
    }
    case MYSQL_TYPE_VARCHAR:
    case MYSQL_TYPE_STRING:
    case MYSQL_TYPE_VAR_STRING: {
      if (is_uuid_field(field))
      {
        auto str= duckdb_value_as_string(value);
        field->store(str.c_str(), str.size(), system_charset_info);
        break;
      }
      if (field->has_charset())
      {
        DBUG_ASSERT(field->charset() != &my_charset_bin);
        auto str= duckdb_value_as_string(value);
        auto varchar= str.c_str();
        auto varchar_len= str.size();
        field->store(varchar, varchar_len, field->charset());
        break;
      }
      else
      {
        auto str= duckdb_value_as_string(value);
        auto varchar= str.c_str();
        auto varchar_len= str.size();
        field->store(varchar, varchar_len, &my_charset_bin);
        break;
      }
    }
    case MYSQL_TYPE_NULL:
    case MYSQL_TYPE_DECIMAL:
    case MYSQL_TYPE_ENUM:
    case MYSQL_TYPE_SET:
    case MYSQL_TYPE_NEWDECIMAL: {
      auto str= duckdb_value_as_string(value);
      auto varchar= str.c_str();
      auto varchar_len= str.size();
      field->store(varchar, varchar_len, system_charset_info);
      break;
    }
    case MYSQL_TYPE_TINY:
    case MYSQL_TYPE_YEAR:
    case MYSQL_TYPE_SHORT:
    case MYSQL_TYPE_INT24:
    case MYSQL_TYPE_LONG:
    case MYSQL_TYPE_LONGLONG:
      store_integer_field(field, value);
      break;
    case MYSQL_TYPE_FLOAT: {
      float v= value.GetValue<float>();
      field->store(v);
      break;
    }
    case MYSQL_TYPE_DOUBLE: {
      double v= value.GetValue<double>();
      field->store(v);
      break;
    }
    case MYSQL_TYPE_DATE:
    case MYSQL_TYPE_NEWDATE: {
      /*
        DuckDB date_t stores days since 1970-01-01.
        Convert to MYSQL_TIME via val_str and store_time.
      */
      auto str= duckdb_value_as_string(value);
      MYSQL_TIME tm;
      MYSQL_TIME_STATUS status;
      my_time_status_init(&status);
      str_to_datetime_or_date(str.c_str(), str.size(), &tm, 0, &status);
      store_field_temporal_value(field, &tm);
      break;
    }
    case MYSQL_TYPE_DATETIME:
    case MYSQL_TYPE_DATETIME2: {
      auto str= duckdb_value_as_string(value);
      MYSQL_TIME tm;
      MYSQL_TIME_STATUS status;
      my_time_status_init(&status);
      str_to_datetime_or_date(str.c_str(), str.size(), &tm, 0, &status);
      store_field_temporal_value(field, &tm);
      break;
    }
    case MYSQL_TYPE_TIMESTAMP:
    case MYSQL_TYPE_TIMESTAMP2: {
      auto str= duckdb_value_as_string(value);
      MYSQL_TIME tm;
      MYSQL_TIME_STATUS status;
      my_time_status_init(&status);
      str_to_datetime_or_date(str.c_str(), str.size(), &tm, 0, &status);
      store_field_temporal_value(field, &tm);
      break;
    }
    case MYSQL_TYPE_TIME:
    case MYSQL_TYPE_TIME2: {
      auto str= duckdb_value_as_string(value);
      MYSQL_TIME tm;
      MYSQL_TIME_STATUS status;
      my_time_status_init(&status);
      str_to_DDhhmmssff(str.c_str(), str.size(), &tm, TIME_MAX_HOUR, &status);
      store_field_temporal_value(field, &tm);
      break;
    }
    default:
      /* TODO: no support */
      DBUG_ASSERT(0);
      break;
    }
  }
  return true;
  }
  catch (const duckdb::Exception &e)
  {
    my_error(ER_GET_ERRMSG, MYF(0), HA_ERR_INTERNAL_ERROR, e.what(),
             "DuckDB");
    return false;
  }
  catch (const std::exception &e)
  {
    my_error(ER_GET_ERRMSG, MYF(0), HA_ERR_INTERNAL_ERROR, e.what(),
             "DuckDB");
    return false;
  }
}
