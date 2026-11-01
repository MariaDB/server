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

#include <my_global.h>
#include "my_time.h"
#include "sql_class.h"
#include "sql_time.h"
#include "tztime.h"

#include "ddl_convertor.h"

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
  Convert a DuckDB TIME value to MYSQL_TIME without DuckDB's string cast.
  physical INT64 and build MYSQL_TIME here instead.
*/
static void store_duckdb_time_in_mysql_format(Field *field, duckdb::Value &value)
{
  MYSQL_TIME tm;
  set_zero_time(&tm, MYSQL_TIMESTAMP_TIME);

  const duckdb::LogicalTypeId type_id= value.type().id();
  if (type_id == duckdb::LogicalTypeId::TIME ||
      type_id == duckdb::LogicalTypeId::TIME_NS)
  {
    int64_t micros;
    if (type_id == duckdb::LogicalTypeId::TIME_NS)
      micros= value.GetValueUnsafe<duckdb::dtime_ns_t>().time().micros;
    else
      micros= value.GetValueUnsafe<duckdb::dtime_t>().micros;
    if (micros < 0)
    {
      tm.neg= 1;
      micros= (micros == LONGLONG_MIN) ? LONGLONG_MAX : -micros;
    }
    const int64_t max_micros=
        (int64_t) TIME_MAX_VALUE_SECONDS * 1000000LL + TIME_MAX_SECOND_PART;
    if (micros > max_micros)
      micros= max_micros;
    calc_time_from_sec(&tm, (ulong) (micros / 1000000LL), (ulong) (micros % 1000000LL));
  }
  else
  {
    auto str= duckdb_value_as_string(value);
    MYSQL_TIME_STATUS status;
    my_time_status_init(&status);
    str_to_DDhhmmssff(str.c_str(), str.size(), &tm, TIME_MAX_HOUR, &status);
  }
  store_field_temporal_value(field, &tm);
}

/**
  Store a duckdb value in a field of mysql format.
  @param field            the field to store value
  @param value            the value to be stored
  @param thd              the thread handle
*/
void store_duckdb_field_in_mysql_format(Field *field, duckdb::Value &value,
                                        THD *thd)
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
    case MYSQL_TYPE_TINY: {
      int64_t v= value.GetValue<int64_t>();
      field->store(v, field->is_unsigned());
      break;
    }
    case MYSQL_TYPE_YEAR:
    case MYSQL_TYPE_SHORT: {
      int64_t v= value.GetValue<int64_t>();
      field->store(v, field->is_unsigned());
      break;
    }
    case MYSQL_TYPE_INT24:
    case MYSQL_TYPE_LONG: {
      int64_t v= value.GetValue<int64_t>();
      field->store(v, field->is_unsigned());
      break;
    }
    case MYSQL_TYPE_LONGLONG: {
      int64_t v;
      if (field->is_unsigned())
      {
        v= value.GetValue<uint64_t>();
      }
      else
      {
        v= value.GetValue<int64_t>();
      }
      field->store(v, field->is_unsigned());
      break;
    }
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
    case MYSQL_TYPE_TIME2:
      store_duckdb_time_in_mysql_format(field, value);
      break;
    default:
      /* TODO: no support */
      DBUG_ASSERT(0);
      break;
    }
  }
}
