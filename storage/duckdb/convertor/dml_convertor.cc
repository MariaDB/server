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

#include "dml_convertor.h"

#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>

#include "sql_table.h" /* primary_key_name */
#include "my_decimal.h"

namespace myduck { extern my_bool use_double_for_decimal; }

static const uint sizeof_trailing_comma= sizeof(", ") - 1;
static const uint sizeof_trailing_and= sizeof(" AND ") - 1;

void append_field_value_to_sql(String &target_str, Field *field)
{
  if (field->is_null())
  {
    target_str.append(STRING_WITH_LEN("NULL"));
    return;
  }

  char field_value_buffer[128];
  String field_value(field_value_buffer, sizeof(field_value_buffer),
                     &my_charset_bin);
  field_value.length(0);

  enum_field_types type= field->real_type();
  switch (type)
  {
  case MYSQL_TYPE_TINY:
  case MYSQL_TYPE_SHORT:
  case MYSQL_TYPE_INT24:
  case MYSQL_TYPE_LONG:
  case MYSQL_TYPE_LONGLONG: {
    field->val_str(&field_value);
    target_str.append(field_value);
    break;
  }
  case MYSQL_TYPE_FLOAT:
  case MYSQL_TYPE_DOUBLE: {
    double value= field->val_real();
    std::stringstream ss;
    ss << std::scientific
       << std::setprecision(std::numeric_limits<double>::max_digits10)
       << value;
    std::string d= ss.str();
    target_str.append(d.c_str(), d.length());
    break;
  }
  case MYSQL_TYPE_NEWDECIMAL: {
    my_decimal value;
    Field_new_decimal *decimal_field= static_cast<Field_new_decimal *>(field);
    uint precision= decimal_field->precision;
    uint8 dec= decimal_field->dec;
    if (precision <= 38)
    {
      decimal_field->val_decimal(&value);
      char buff[DECIMAL_MAX_STR_LENGTH + 1];
      int string_length= DECIMAL_MAX_STR_LENGTH + 1;
      decimal2string(&value, buff, &string_length, precision, dec, '0');
      target_str.append(buff, string_length);
    }
    else if (myduck::use_double_for_decimal)
    {
      /* DuckDB column is DOUBLE for precision >38 — emit as double literal */
      char buff[64];
      snprintf(buff, sizeof(buff), "%.17e", field->val_real());
      target_str.append(buff, strlen(buff));
    }
    else
    {
      field->val_str(&field_value);
      target_str.append(field_value);
    }
    break;
  }
  case MYSQL_TYPE_DATE:
  case MYSQL_TYPE_NEWDATE:
  case MYSQL_TYPE_DATETIME:
  case MYSQL_TYPE_DATETIME2:
  case MYSQL_TYPE_YEAR:
  case MYSQL_TYPE_TIME:
  case MYSQL_TYPE_TIME2: {
    target_str.append(STRING_WITH_LEN("'"));
    field->val_str(&field_value);
    target_str.append(field_value);
    target_str.append(STRING_WITH_LEN("'"));
    break;
  }
  case MYSQL_TYPE_TIMESTAMP:
  case MYSQL_TYPE_TIMESTAMP2: {
    target_str.append(STRING_WITH_LEN("'"));
    field->val_str(&field_value);
    target_str.append(field_value);
    target_str.append(STRING_WITH_LEN("'"));
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
    field->val_str(&field_value);
    std::string hex_str= toHex(field_value.c_ptr_safe(), field_value.length());

    if (FieldConvertor::convert_type(field) == "BLOB")
    {
      target_str.append(hex_str.c_str(), hex_str.size());
    }
    else
    {
      target_str.append(STRING_WITH_LEN("DECODE("));
      target_str.append(hex_str.c_str(), hex_str.size());
      target_str.append(STRING_WITH_LEN(")::VARCHAR"));
    }
    break;
  }
  default:
    target_str.append(STRING_WITH_LEN("__ERROR__"));
  }
}

static inline void append_table_name(TABLE *table, String &query)
{
  /*
    Use normalized_path to resolve the actual DuckDB table name.
    During ALTER TABLE, table->s->table_name is the final name (e.g.
    "t_tinyint") but the DuckDB table is the temp "#sql-alter-..." table
    created by ha_duckdb::create(). The normalized_path correctly contains
    the temp name.
  */
  DatabaseTableNames dt(table->s->normalized_path.str);
  query.append(STRING_WITH_LEN("\""));
  query.append(dt.db_name.c_str(), dt.db_name.length());
  query.append(STRING_WITH_LEN("\""));
  query.append(STRING_WITH_LEN("."));
  query.append(STRING_WITH_LEN("\""));
  query.append(dt.table_name.c_str(), dt.table_name.length());
  query.append(STRING_WITH_LEN("\""));
}

static inline void get_write_fields(TABLE *table, std::vector<Field *> &fields)
{
  for (uint i= 0; i < table->s->fields; i++)
  {
    Field *field= table->field[i];
    if (bitmap_is_set(table->write_set, field->field_index))
    {
      fields.push_back(field);
    }
  }
}

std::string DMLConvertor::translate()
{
  char query_buffer[128];
  String query(query_buffer, sizeof(query_buffer), &my_charset_bin);
  query.length(0);

  generate_prefix(query);
  generate_fields_and_values(query);
  generate_where_clause(query);

  return (std::string(query.c_ptr_safe(), query.length()));
}

void DMLConvertor::fill_index_fields_for_where(std::vector<Field *> &fields)
{
  KEY *key_info= m_table->key_info;
  if (key_info)
  {
    KEY_PART_INFO *key_part= key_info->key_part;
    for (uint j= 0; j < key_info->user_defined_key_parts; j++, key_part++)
    {
      fields.push_back(key_part->field);
    }
  }
  else
  {
    for (uint j= 0; j < m_table->s->fields; j++)
    {
      fields.push_back(m_table->field[j]);
    }
  }
}

void DMLConvertor::generate_where_clause(String &query)
{
  std::vector<Field *> fields;
  fill_index_fields_for_where(fields);
  assert(fields.size());

  if (!fields.size())
    return;

  query.append(STRING_WITH_LEN(" WHERE "));

  for (auto field : fields)
  {
    query.append(STRING_WITH_LEN("\""));
    query.append(field->field_name.str, field->field_name.length);
    query.append(STRING_WITH_LEN("\""));
    query.append(STRING_WITH_LEN(" = "));

    append_where_value(query, field);

    query.append(STRING_WITH_LEN(" AND "));
  }
  query.length(query.length() - sizeof_trailing_and);
}

void InsertConvertor::generate_prefix(String &query)
{
  query.append(STRING_WITH_LEN("INSERT INTO "));
  append_table_name(m_table, query);
}

void InsertConvertor::generate_fields_and_values(String &query)
{
  std::vector<Field *> fields;
  get_write_fields(m_table, fields);

  if (fields.size())
  {
    query.append(STRING_WITH_LEN(" ("));
    for (auto field : fields)
    {
      query.append(STRING_WITH_LEN("\""));
      query.append(field->field_name.str, field->field_name.length);
      query.append(STRING_WITH_LEN("\""));
      query.append(STRING_WITH_LEN(", "));
    }
    query.length(query.length() - sizeof_trailing_comma);
    query.append(STRING_WITH_LEN(")"));
  }

  query.append(STRING_WITH_LEN(" VALUES ("));
  for (auto field : fields)
  {
    append_field_value_to_sql(query, field);
    query.append(STRING_WITH_LEN(", "));
  }
  query.length(query.length() - sizeof_trailing_comma);
  query.append(STRING_WITH_LEN(")"));
}

void UpdateConvertor::generate_prefix(String &query)
{
  query.append(STRING_WITH_LEN("UPDATE "));
  append_table_name(m_table, query);
  query.append(STRING_WITH_LEN(" SET "));
}

void UpdateConvertor::generate_fields_and_values(String &query)
{
  std::vector<Field *> fields;
  get_write_fields(m_table, fields);

  for (auto field : fields)
  {
    query.append(STRING_WITH_LEN("\""));
    query.append(field->field_name.str, field->field_name.length);
    query.append(STRING_WITH_LEN("\""));
    query.append(STRING_WITH_LEN(" = "));

    append_field_value_to_sql(query, field);
    query.append(STRING_WITH_LEN(", "));
  }
  query.length(query.length() - sizeof_trailing_comma);
}

void DeleteConvertor::generate_prefix(String &query)
{
  query.append(STRING_WITH_LEN("DELETE FROM "));
  append_table_name(m_table, query);
}
