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

#include <my_global.h>
#include "sql_class.h"
#include "log.h"

#undef UNKNOWN

#include "delta_appender.h"
#include "duckdb_query.h"
#include "duckdb_config.h"
#include "ddl_convertor.h"
#include "duckdb_timezone.h"
#include "duckdb_handler_errors.h"
#include "tztime.h"
#include "my_decimal.h"

#include "duckdb/common/hugeint.hpp"
#include "duckdb/common/types/decimal.hpp"

#include <sstream>

#define DIG_PER_DEC1 9
#define DIG_BASE 1000000000
#define ROUND_UP(X) (((X) + DIG_PER_DEC1 - 1) / DIG_PER_DEC1)
static const decimal_digit_t powers10[DIG_PER_DEC1 + 1]= {
    1, 10, 100, 1000, 10000, 100000, 1000000, 10000000, 100000000, 1000000000};

static int my_decimal_actual_intg(const my_decimal *from)
{
  int intg= from->intg;
  const decimal_digit_t *buf= from->buf;
  if (intg == 0)
    return 0;
  int complete_groups= ROUND_UP(intg);
  int i= 0;
  while (i < complete_groups && buf[i] == 0)
    i++;
  if (i >= complete_groups)
    return 0;
  int digits= 0;
  decimal_digit_t temp= buf[i];
  while (temp > 0)
  {
    temp/= 10;
    digits++;
  }
  return digits + (complete_groups - i - 1) * DIG_PER_DEC1;
}

template <typename T>
static T get_duckdb_decimal(const my_decimal &from, int fixed_decimal)
{
  T res{0};
  decimal_digit_t *buf= from.buf;
  int intg= from.intg, frac= from.frac, fill= fixed_decimal - frac;
  bool sign= from.sign();

  auto update_res= [&](longlong digit) {
    if (sign)
      res-= digit;
    else
      res+= digit;
  };

  for (; intg > 0; intg-= DIG_PER_DEC1)
  {
    res*= DIG_BASE;
    update_res(*buf++);
  }

  for (; frac >= DIG_PER_DEC1; frac-= DIG_PER_DEC1)
  {
    res*= DIG_BASE;
    update_res(*buf++);
  }

  if (frac > 0)
  {
    res*= powers10[frac];
    update_res(*buf / powers10[DIG_PER_DEC1 - frac]);
  }

  if (fill > 0)
    res*= powers10[fill];

  return res;
}

int DeltaAppender::append_row_insert(TABLE *table, ulonglong trx_no,
                                     const MY_BITMAP *blob_type_map)
{
  ++m_row_count;
  m_has_insert= true;

  try
  {
    m_appender->BeginRow();

    for (uint i= 0; i < table->s->fields; i++)
    {
      int ret= append_mysql_field(table->field[i], blob_type_map);
      if (ret)
        return HA_DUCKDB_APPEND_ERROR;
    }

    if (m_use_tmp_table)
    {
      m_appender->Append<int64_t>(0);
      m_appender->Append<int64_t>(m_row_count);
      m_appender->Append<int64_t>(trx_no);
    }

    m_appender->EndRow();
  }
  catch (std::exception &ex)
  {
    sql_print_error("DuckDB: Appender error: %s", ex.what());
    my_error(ER_GET_ERRMSG, MYF(0), HA_DUCKDB_APPEND_ERROR, ex.what(), "DuckDB Appender");
    return HA_DUCKDB_APPEND_ERROR;
  }

  return 0;
}

int DeltaAppender::append_row_update(TABLE *table, ulonglong trx_no,
                                     const uchar *old_row)
{
  m_has_update= true;
  return (append_row_delete(table, trx_no, old_row) ||
          append_row_insert(table, trx_no, nullptr))
             ? HA_DUCKDB_APPEND_ERROR
             : 0;
}

int DeltaAppender::append_row_delete(TABLE *table, ulonglong trx_no,
                                     const uchar *old_row)
{
  ++m_row_count;
  m_has_delete= true;

  try
  {
    m_appender->BeginRow();

    for (uint i= 0; i < table->s->fields; i++)
    {
      Field *field= table->field[i];

      if (bitmap_is_set(&m_pk_bitmap, field->field_index))
      {
        int ret= 0;
        if (!old_row)
        {
          ret= append_mysql_field(field);
        }
        else
        {
          uchar *saved_ptr= field->ptr;
          field->ptr=
              const_cast<uchar *>(old_row + field->offset(table->record[0]));
          ret= append_mysql_field(field);
          field->ptr= saved_ptr;
        }
        if (ret)
          return HA_DUCKDB_APPEND_ERROR;
      }
      else
      {
        m_appender->Append(duckdb::Value(duckdb::LogicalType::SQLNULL));
      }
    }

    if (m_use_tmp_table)
    {
      m_appender->Append<int64_t>(1);
      m_appender->Append<int64_t>(m_row_count);
      m_appender->Append<int64_t>(trx_no);
    }

    m_appender->EndRow();
  }
  catch (std::exception &ex)
  {
    sql_print_error("DuckDB: Appender error: %s", ex.what());
    my_error(ER_GET_ERRMSG, MYF(0), HA_DUCKDB_APPEND_ERROR, ex.what(), "DuckDB Appender");
    return HA_DUCKDB_APPEND_ERROR;
  }

  return 0;
}

bool DeltaAppender::Initialize(TABLE *table)
{
  if (m_use_tmp_table)
  {
    m_tmp_table_name= buf_table_name(m_schema_name, m_table_name);

    std::stringstream ss;
    ss << "CREATE TEMPORARY TABLE IF NOT EXISTS main.\"" << m_tmp_table_name
       << "\" AS FROM \"" << m_schema_name << "\".\"" << m_table_name
       << "\" LIMIT 0;";
    ss << "ALTER TABLE main.\"" << m_tmp_table_name
       << "\" ADD COLUMN \"#mdb_delete_flag\" BOOL;";
    ss << "ALTER TABLE main.\"" << m_tmp_table_name
       << "\" ADD COLUMN \"#mdb_row_no\" INT;";
    ss << "ALTER TABLE main.\"" << m_tmp_table_name
       << "\" ADD COLUMN \"#mdb_trx_no\" INT;";

    auto ret= myduck::duckdb_query(*m_con, ss.str());
    if (ret->HasError())
      return true;

    std::string schema_name("main");
    try
    {
      m_appender= std::make_unique<duckdb::Appender>(*m_con, schema_name,
                                                     m_tmp_table_name);
    }
    catch (std::exception &ex)
    {
      sql_print_error("DuckDB: Appender init error (tmp): %s", ex.what());
      return true;
    }

    KEY *key_info= table->key_info;
    if (!key_info)
      return true;
    my_bitmap_init(&m_pk_bitmap, nullptr, table->s->fields);
    KEY_PART_INFO *key_part= key_info->key_part;
    for (uint i= 0; i < key_info->user_defined_key_parts; i++, key_part++)
    {
      if (i)
        m_pk_list+= ", ";
      m_pk_list+= "\"";
      m_pk_list+= key_part->field->field_name.str;
      m_pk_list+= "\"";
      bitmap_set_bit(&m_pk_bitmap, key_part->field->field_index);
    }

    for (uint i= 0; i < table->s->fields; i++)
    {
      if (i)
        m_col_list+= ", ";
      m_col_list+= "\"";
      m_col_list+= table->field[i]->field_name.str;
      m_col_list+= "\"";
    }
  }
  else
  {
    try
    {
      m_appender= std::make_unique<duckdb::Appender>(*m_con, m_schema_name,
                                                     m_table_name);
    }
    catch (std::exception &ex)
    {
      sql_print_error("DuckDB: Appender init error: %s", ex.what());
      return true;
    }
  }

  return false;
}

int DeltaAppender::append_mysql_field(const Field *field_arg,
                                      const MY_BITMAP *blob_type_map)
{
  Field *field= const_cast<Field *>(field_arg);
  auto appender= m_appender.get();

  if (field->is_real_null())
  {
    appender->Append(duckdb::Value(duckdb::LogicalType::SQLNULL));
    return 0;
  }

  enum_field_types type= field->real_type();

  switch (type)
  {
  case MYSQL_TYPE_TINY:
  case MYSQL_TYPE_SHORT:
  case MYSQL_TYPE_INT24:
  case MYSQL_TYPE_LONG: {
    longlong value= field->val_int();
    appender->Append<int64_t>(value);
    break;
  }
  case MYSQL_TYPE_LONGLONG: {
    longlong value= field->val_int();
    if (field->is_unsigned())
      appender->Append<uint64_t>(value);
    else
      appender->Append<int64_t>(value);
    break;
  }
  case MYSQL_TYPE_FLOAT: {
    float value= static_cast<float>(field->val_real());
    appender->Append<float>(value);
    break;
  }
  case MYSQL_TYPE_DOUBLE: {
    double value= field->val_real();
    appender->Append<double>(value);
    break;
  }
  case MYSQL_TYPE_NEWDECIMAL: {
    my_decimal value;
    Field_new_decimal *decimal_field= static_cast<Field_new_decimal *>(field);
    uint precision_val= decimal_field->precision;
    uint8 dec= decimal_field->dec;

    if (precision_val <= 38)
    {
      decimal_field->val_decimal(&value);
      if (value.intg + value.frac > (int) precision_val ||
          value.frac > (int) dec)
      {
        my_error(ER_GET_ERRMSG, MYF(0), HA_DUCKDB_APPEND_ERROR, "Append DECIMAL field failed", "DuckDB Appender");
        return HA_DUCKDB_APPEND_ERROR;
      }

      if (precision_val <= duckdb::Decimal::MAX_WIDTH_INT16)
        appender->Append(duckdb::Value::DECIMAL(
            get_duckdb_decimal<int16_t>(value, dec), precision_val, dec));
      else if (precision_val <= duckdb::Decimal::MAX_WIDTH_INT32)
        appender->Append(duckdb::Value::DECIMAL(
            get_duckdb_decimal<int32_t>(value, dec), precision_val, dec));
      else if (precision_val <= duckdb::Decimal::MAX_WIDTH_INT64)
        appender->Append(duckdb::Value::DECIMAL(
            get_duckdb_decimal<int64_t>(value, dec), precision_val, dec));
      else
        appender->Append(duckdb::Value::DECIMAL(
            get_duckdb_decimal<duckdb::hugeint_t>(value, dec), precision_val,
            dec));
    }
    else if (myduck::use_double_for_decimal)
    {
      double dval= decimal_field->val_real();
      appender->Append<double>(dval);
    }
    else
    {
      /* Append as decimal(38, dec) — truncate intg to fit */
      decimal_field->val_decimal(&value);
      int real_intg= my_decimal_actual_intg(&value);
      if (real_intg + (int) dec > 38)
      {
        my_error(ER_GET_ERRMSG, MYF(0), HA_DUCKDB_APPEND_ERROR,
                 "Decimal value out of range for DECIMAL(38,...)", "DuckDB Appender");
        return HA_DUCKDB_APPEND_ERROR;
      }
      appender->Append(duckdb::Value::DECIMAL(
          get_duckdb_decimal<duckdb::hugeint_t>(value, dec), 38, dec));
    }
    break;
  }
  case MYSQL_TYPE_DATE:
  case MYSQL_TYPE_NEWDATE: {
    MYSQL_TIME tm;
    field->get_date(&tm, date_mode_t(0));
    long date=
        calc_daynr(tm.year, tm.month, tm.day) - myduck::days_at_timestart;
    appender->Append<duckdb::date_t>(static_cast<duckdb::date_t>(date));
    break;
  }
  case MYSQL_TYPE_DATETIME:
  case MYSQL_TYPE_DATETIME2: {
    MYSQL_TIME tm;
    field->get_date(&tm, date_mode_t(0));
    /* Compute microseconds since Unix epoch directly, supporting
       dates before 1970 (which TIME_to_gmt_sec cannot handle). */
    long days= calc_daynr(tm.year, tm.month, tm.day) - calc_daynr(1970, 1, 1);
    longlong secs=
        days * 86400LL + tm.hour * 3600LL + tm.minute * 60LL + tm.second;
    appender->Append<duckdb::timestamp_t>(
        static_cast<duckdb::timestamp_t>(secs * 1000000LL + tm.second_part));
    break;
  }
  case MYSQL_TYPE_YEAR: {
    longlong value= field->val_int();
    appender->Append<int64_t>(value);
    break;
  }
  case MYSQL_TYPE_TIME:
  case MYSQL_TYPE_TIME2: {
    MYSQL_TIME tm;
    field->get_date(&tm, date_mode_t(0));
    appender->Append<duckdb::dtime_t>(static_cast<duckdb::dtime_t>(
        (tm.hour * 3600LL + tm.minute * 60LL + tm.second) * 1000000LL +
        tm.second_part));
    break;
  }
  case MYSQL_TYPE_TIMESTAMP:
  case MYSQL_TYPE_TIMESTAMP2: {
    /* Use get_date() to get local-time representation, matching the
       non-batch SQL path which stores val_str() (local time string).
       get_timestamp() returns UTC, but DuckDB read path does not
       apply timezone conversion, so we must store local time. */
    MYSQL_TIME tm;
    field->get_date(&tm, date_mode_t(0));
    long days= calc_daynr(tm.year, tm.month, tm.day) - calc_daynr(1970, 1, 1);
    longlong secs=
        days * 86400LL + tm.hour * 3600LL + tm.minute * 60LL + tm.second;
    appender->Append<duckdb::timestamp_t>(
        static_cast<duckdb::timestamp_t>(secs * 1000000LL + tm.second_part));
    break;
  }
  case MYSQL_TYPE_SET:
  case MYSQL_TYPE_ENUM:
  case MYSQL_TYPE_BIT:
  case MYSQL_TYPE_GEOMETRY:
  case MYSQL_TYPE_VARCHAR:
  case MYSQL_TYPE_STRING:
  case MYSQL_TYPE_TINY_BLOB:
  case MYSQL_TYPE_BLOB:
  case MYSQL_TYPE_MEDIUM_BLOB:
  case MYSQL_TYPE_LONG_BLOB: {
    char buf[128];
    String tmp(buf, sizeof(buf), &my_charset_bin);
    field->val_str(&tmp);

    if (is_uuid_field(field))
    {
      appender->Append(
          duckdb::Value::UUID(std::string(tmp.ptr(), tmp.length())));
      break;
    }

    bool is_blob= false;
    if (blob_type_map != nullptr)
      is_blob= bitmap_is_set(blob_type_map, field->field_index);
    else
      is_blob= (FieldConvertor::convert_type(field) == "BLOB");

    if (is_blob)
    {
      auto value= duckdb::Value::BLOB((duckdb::const_data_ptr_t) tmp.ptr(),
                                      tmp.length());
      appender->Append(value);
    }
    else
    {
      appender->Append<duckdb::string_t>(
          duckdb::string_t(tmp.ptr(), tmp.length()));
    }
    break;
  }
  default:
    return HA_DUCKDB_APPEND_ERROR;
  }
  return 0;
}

static void appendSelectQuery(std::stringstream &ss,
                              const std::string &select_list,
                              const std::string &pk_list,
                              const std::string &table_name, int delete_flag)
{
  ss << "SELECT UNNEST(r) FROM (SELECT LAST(ROW(" << select_list
     << ") ORDER BY \"#mdb_row_no\") AS r, "
        "LAST(\"#mdb_delete_flag\" ORDER BY \"#mdb_row_no\") AS "
        "\"#mdb_delete_flag\" FROM main.\""
     << table_name << "\" GROUP BY " << pk_list << ")";
  if (!delete_flag)
    ss << " WHERE \"#mdb_delete_flag\" = " << delete_flag;
}

void DeltaAppender::generateQuery(std::stringstream &ss, bool delete_flag)
{
  ss.str("");
  ss << "USE \"" << m_schema_name << "\"; ";

  if (!delete_flag)
  {
    ss << "INSERT INTO \"" << m_schema_name << "\".\"" << m_table_name
       << "\" ";
    appendSelectQuery(ss, m_col_list, m_pk_list, m_tmp_table_name,
                      delete_flag);
    ss << ";";
  }
  else
  {
    ss << "DELETE FROM \"" << m_schema_name << "\".\"" << m_table_name
       << "\" WHERE (" << m_pk_list << ") IN (";
    appendSelectQuery(ss, m_pk_list, m_pk_list, m_tmp_table_name, delete_flag);
    ss << ");";
  }
}

bool DeltaAppender::flush(bool idempotent_flag)
{
  m_appender->Flush();

  if (m_use_tmp_table)
  {
    std::stringstream ss;

    if (m_has_delete || idempotent_flag)
    {
      generateQuery(ss, true);
      auto ret= myduck::duckdb_query(*m_con, ss.str());
      if (ret->HasError())
        return true;
    }

    if (m_has_insert)
    {
      generateQuery(ss, false);
      auto ret= myduck::duckdb_query(*m_con, ss.str());
      if (ret->HasError())
        return true;
    }

    ss.str("");
    ss << "DROP TABLE main.\"" << m_tmp_table_name << "\"";
    auto ret= myduck::duckdb_query(*m_con, ss.str());
    if (ret->HasError())
      return true;
  }

  return false;
}

void DeltaAppender::cleanup()
{
  if (m_use_tmp_table)
  {
    my_bitmap_free(&m_pk_bitmap);
    std::stringstream ss;
    ss << "DROP TABLE IF EXISTS main.\"" << m_tmp_table_name << "\";";
    myduck::duckdb_query(*m_con, ss.str());
  }
}

void DeltaAppenders::delete_appender(std::string &db, std::string &tb)
{
  auto key= std::make_pair(db, tb);
  m_append_infos.erase(key);
}

bool DeltaAppenders::flush_all(bool idempotent_flag, std::string &error_msg)
{
  try
  {
    for (auto &pair : m_append_infos)
    {
      if (pair.second->flush(idempotent_flag))
      {
        error_msg= "DeltaAppender flush failed";
        return true;
      }
    }
  }
  catch (std::exception &ex)
  {
    error_msg= ex.what();
    sql_print_error("DuckDB: DeltaAppender flush error: %s",
                    error_msg.c_str());
    return true;
  }
  m_append_infos.clear();
  return false;
}

DeltaAppender *DeltaAppenders::get_appender(std::string &db, std::string &tb,
                                            bool insert_only, TABLE *table)
{
  auto key= std::make_pair(db, tb);
  auto it= m_append_infos.find(key);
  if (it != m_append_infos.end())
    return it->second.get();

  auto appender= std::make_unique<DeltaAppender>(m_con, db, tb, !insert_only);
  try
  {
    if (appender->Initialize(table))
      return nullptr;
  }
  catch (std::exception &ex)
  {
    sql_print_error("DuckDB: DeltaAppender init error: %s", ex.what());
    return nullptr;
  }

  auto *raw= appender.get();
  m_append_infos[key]= std::move(appender);
  return raw;
}
