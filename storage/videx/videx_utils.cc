/* Copyright (c) 2024 Bytedance Ltd. and/or its affiliates

  This program is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License, version 2.0,
  as published by the Free Software Foundation.

  This program is also distributed with certain software (including
  but not limited to OpenSSL) that is licensed under separate terms,
  as designated in a particular file or component or in included license
  documentation.  The authors of MySQL hereby grant you an additional
  permission to link the program and your derivative works with the
  separately licensed software that they have included with MySQL.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License, version 2.0, for more details.

  You should have received a copy of the GNU General Public License
  along with this program; if not, write to the Free Software
  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA */

#include "videx_utils.h"
#include <mysql/service_thd_alloc.h>

/**
 * It's to provide a simple but robust parsing function here,
 * since rapid_json always encounters strange segmentation faults across
 * platforms, especially on MacOS.
 *
 * @param json
 * @param code
 * @param message
 * @param data_dict
 * @return
 */

int videx_parse_simple_json(const std::string &json, int &code,
                            std::string &message,
                            std::map<std::string, std::string> &data_dict)
{
  try
  {
    // find code and message
    std::size_t pos_code= json.find("\"code\":");
    std::size_t pos_message= json.find("\"message\":");
    std::size_t pos_data= json.find("\"data\":");

    if (pos_code == std::string::npos || pos_message == std::string::npos ||
        pos_data == std::string::npos)
    {
      throw std::invalid_argument("Missing essential components in JSON.");
    }

    // parse code
    std::size_t start= json.find_first_of("0123456789", pos_code);
    std::size_t end= json.find(',', start);
    code= std::stoi(json.substr(start, end - start));

    // parse message
    start= json.find('\"', pos_message + 10) + 1;
    end= json.find('\"', start);
    message= json.substr(start, end - start);

    // parse data
    start= json.find('{', pos_data) + 1;
    end= json.find('}', start);
    std::string data_content= json.substr(start, end - start);
    std::istringstream data_stream(data_content);
    std::string line;

    while (std::getline(data_stream, line, ','))
    {
      std::size_t colon_pos= line.find(':');
      if (colon_pos == std::string::npos)
      {
        continue; // Skip malformed line
      }
      std::string key= line.substr(0, colon_pos);
      std::string value= line.substr(colon_pos + 1);

      // clean key and value
      auto trim_quotes_and_space= [](std::string &str) {
        size_t first= str.find_first_not_of(" \t\n\"");
        size_t last= str.find_last_not_of(" \t\n\"");
        if (first == std::string::npos || last == std::string::npos)
        {
          str.clear();
        }
        else
        {
          str= str.substr(first, last - first + 1);
        }
      };

      trim_quotes_and_space(key);
      trim_quotes_and_space(value);

      data_dict[key]= value;
    }

    return 0;
  }
  catch (std::exception &e)
  {
    std::cerr << "Failed to parse JSON: " << e.what() << std::endl;
    message= e.what();
    code= -1;
    return 1;
  }
}

/**
 * This function is used to escape double quotes in a string.
 * @param input
 * @param len
 * @return
 */

std::string videx_escape_double_quotes(const std::string &input, size_t len)
{
  if (len == std::string::npos)
    len= input.length();

  std::string output= input.substr(0, len);
  size_t pos= output.find('\\');
  while (pos != std::string::npos)
  {
    output.replace(pos, 1, "\\\\");
    pos= output.find('\\', pos + 2);
  }
  pos= output.find('\"');
  while (pos != std::string::npos)
  {
    output.replace(pos, 1, "\\\"");
    pos= output.find('\"', pos + 2);
  }

  pos= output.find('\n');
  while (pos != std::string::npos)
  {
    output.replace(pos, 1, " ");
    pos= output.find('\n', pos + 1);
  }

  pos= output.find('\t');
  while (pos != std::string::npos)
  {
    output.replace(pos, 1, " ");
    pos= output.find('\t', pos + 1);
  }
  return output;
}

VidexJsonItem *VidexJsonItem::create(const std::string &new_item_type)
{
  data.push_back(VidexJsonItem(new_item_type, depth + 1));
  return &data.back();
}

VidexJsonItem *VidexJsonItem::create(const std::string &item_type, const char *prompt)
{
  VidexJsonItem newOne= VidexJsonItem(item_type, depth + 1);
  newOne.add_property("prompt", prompt);
  data.push_back(newOne);
  return &data.back();
}

void VidexJsonItem::add_property(const std::string &key, const std::string &value)
{
  properties[key]= videx_escape_double_quotes(value);
}

void VidexJsonItem::add_property(const std::string &key, const char *value)
{
  if (value != NULL)
  {
    properties[key]= videx_escape_double_quotes(value);
  }
  else
  {
    properties[key]= "NULL";
  }
}

void VidexJsonItem::add_property(const std::string &key, const String &value)
{
  if (!value.is_alloced() || !value.ptr() || !value.alloced_length() ||
      (value.alloced_length() < (value.length() + 1)))
  {
    properties[key]= "NULL";
  }
  else
  {
    properties[key]= videx_escape_double_quotes(value.ptr(), value.length());
  }
}

void VidexJsonItem::add_property(const std::string &key, const String *value)
{
  if (value == NULL)
  {
    properties[key]= "NULL";
  }
  else
  {
    add_property(key, *value);
  }
}

std::string VidexJsonItem::to_json() const
{
  std::string json= "{";

  json+= "\"item_type\":\"" + item_type + "\",";

  json+= "\"properties\":{";
  for (std::map<std::string, std::string>::const_iterator it=
           properties.begin();
       it != properties.end(); ++it)
  {
    json+= "\"" + it->first + "\":\"" + it->second + "\",";
  }
  if (!properties.empty())
  {
    json.erase(json.length() - 1); // remove trailing comma
  }
  json+= "},";

  json+= "\"data\":[";
  for (std::list<VidexJsonItem>::const_iterator it= data.begin();
       it != data.end(); ++it)
  {
    json+= it->to_json() + ",";
  }
  if (!data.empty())
  {
    json.erase(json.length() - 1); // remove trailing comma
  }
  json+= "]}";

  return json;
}

/**
Return printable field name; MariaDB 11.0 lacks functional index names.
*/

const char *get_field_name_or_expression(const Field *field)
{
  return field->field_name.str;
}

/**
  Print a key to a string
  referring to print_key_value - sql/range_optimizer/range_optimizer.cc:1429

  @param[out] out          String the key is appended to
  @param[in]  key_part     Index components description
  @param[in]  key          Key tuple
*/

void videx_print_key_value(String *out, const KEY_PART_INFO *key_part,
                           const uchar *uchar_key)
{
  Field *field= key_part->field;

  if (field->flags & BLOB_FLAG)
  {
    // Byte 0 of a nullable key is the null-byte. If set, key is NULL.
    if (field->maybe_null() && *uchar_key)
    {
      out->append(STRING_WITH_LEN("NULL"));
      return;
    }
    else if (field->type() == MYSQL_TYPE_GEOMETRY)
    {
      out->append(STRING_WITH_LEN("unprintable_geometry_value"));
      return;
    }
    else
    {
      // if uncomment, videx will return fixed "unprintable_blob_value"
      // out->append(STRING_WITH_LEN("unprintable_blob_value"));
      // return;
    }
  }

  uint store_length= key_part->store_length;

  if (field->maybe_null())
  {
    /*
      Byte 0 of key is the null-byte. If set, key is NULL.
      Otherwise, print the key value starting immediately after the
      null-byte
    */
    if (*uchar_key)
    {
      out->append(STRING_WITH_LEN("NULL"));
      return;
    }
    uchar_key++; // Skip null byte
    store_length--;
  }

  /*
    Binary data cannot be converted to UTF8 which is what the
    optimizer trace expects. If the column is binary, the hex
    representation is printed to the trace instead.
  */
  if (field->result_type() == STRING_RESULT &&
      field->charset() == &my_charset_bin)
  {
    out->append(STRING_WITH_LEN("0x"));
    for (uint i= 0; i < store_length; i++)
    {
      out->append(_dig_vec_lower[*(uchar_key + i) >> 4]);
      out->append(_dig_vec_lower[*(uchar_key + i) & 0x0F]);
    }
    return;
  }

  StringBuffer<128> tmp(system_charset_info);
  bool add_quotes= field->result_type() == STRING_RESULT;

  TABLE *table= field->table;
  MY_BITMAP *old_sets[2];

  dbug_tmp_use_all_columns(table, old_sets, &table->read_set,
                           &table->write_set);

  field->set_key_image(uchar_key, key_part->length);
  if (field->type() == MYSQL_TYPE_BIT)
  {
    (void) field->val_int_as_str(&tmp, true); // may change tmp's charset
    add_quotes= false;
  }
  else
  {
    field->val_str(&tmp); // may change tmp's charset
  }

  dbug_tmp_restore_column_maps(&table->read_set, &table->write_set, old_sets);

  if (add_quotes)
  {
    out->append('\'');
    // Worst case: Every character is escaped.
    const size_t buffer_size= tmp.length() * 2 + 1;
    char *quoted_string= (char *) thd_alloc(current_thd, buffer_size);

    my_bool overflow;
    const size_t quoted_length=
        escape_string_for_mysql(tmp.charset(), quoted_string, buffer_size,
                                tmp.ptr(), tmp.length(), &overflow);
    if (overflow)
    {
      // Overflow. Our worst case estimate for the buffer size was too low.
      assert(false);
      return;
    }
    out->append(quoted_string, quoted_length, tmp.charset());
    out->append('\'');
  }
  else
  {
    out->append(tmp.ptr(), tmp.length(), tmp.charset());
  }
}

/**
Convert range read function to a concise symbolic operator string.
*/

std::string haRKeyFunctionToSymbol(ha_rkey_function function)
{
  switch (function)
  {
  case HA_READ_KEY_EXACT:
    return "=";
  case HA_READ_KEY_OR_NEXT:
    return ">=";
  case HA_READ_KEY_OR_PREV:
    return "<=";
  case HA_READ_AFTER_KEY:
    return ">";
  case HA_READ_BEFORE_KEY:
    return "<";
  case HA_READ_PREFIX:
    return "=x%";
  case HA_READ_PREFIX_LAST:
    return "last_x%";
  case HA_READ_PREFIX_LAST_OR_PREV:
    return "<=last_x%";
  case HA_READ_MBR_CONTAIN:
    return "HA_READ_MBR_CONTAIN";
  case HA_READ_MBR_INTERSECT:
    return "HA_READ_MBR_INTERSECT";
  case HA_READ_MBR_WITHIN:
    return "HA_READ_MBR_WITHIN";
  case HA_READ_MBR_DISJOINT:
    return "HA_READ_MBR_DISJOINT";
  case HA_READ_MBR_EQUAL:
    return "HA_READ_MBR_EQUAL";
  default:
    return "Unknown ha_rkey_function";
  }
}

/**
Append one column bound to output and JSON; used by key-range serialization.
*/

inline void subha_append_range(String *out, const KEY_PART_INFO *key_part,
                               const uchar *uchar_key, const uint,
                               VidexJsonItem *range_json)
{
  if (out->length() > 0)
    out->append(STRING_WITH_LEN("  "));
  String tmp_str;
  tmp_str.set_charset(system_charset_info);
  tmp_str.length(0);
  std::stringstream ss;

  const char *field_or_expr=get_field_name_or_expression(key_part->field);
  out->append(field_or_expr, strlen(field_or_expr));
  range_json->add_property("column", field_or_expr);

  out->append(STRING_WITH_LEN("("));
  videx_print_key_value(&tmp_str, key_part, uchar_key);
  out->append(tmp_str);
  out->append(STRING_WITH_LEN("), "));
  ss.write(tmp_str.ptr(), tmp_str.length());
  range_json->add_property("value", ss.str());
  tmp_str.length(0);
}

/**
Return indices of set bits (0..63) in the given bitmap.
*/

std::vector<int> BitsSetIn(ulong bitmap)
{
  std::vector<int> result;
  for (int i= 0; i < 64; ++i)
  {
    if (bitmap & (1UL << i))
      result.push_back(i);
  }
  return result;
}

/**
Serialize a `key_range` into text and JSON; mirrors range optimizer output.
*/

void subha_parse_key_range(const key_range *key_range, const KEY *index,
                           String *out, VidexJsonItem *req_json)
{
  const uint QUICK_RANGE_flag= -1;
  if (key_range == nullptr)
  {
    out->append(STRING_WITH_LEN("<NO_KEY_RANGE>"));
    return;
  }
  KEY_PART_INFO *first_key_part= index->key_part;
  out->append(STRING_WITH_LEN(" "));
  std::string key_range_flag_str= haRKeyFunctionToSymbol(key_range->flag);
  out->append(key_range_flag_str.c_str(), key_range_flag_str.length());

  req_json->add_property("operator", key_range_flag_str);
  req_json->add_property_nonan("length", key_range->length);
  req_json->add_property("index_name", index->name.str);

  const uchar *uchar_key= key_range->key;
  for (int keypart_idx : BitsSetIn(key_range->keypart_map))
  {
    VidexJsonItem *range_json= req_json->create("column_and_bound");
    subha_append_range(out, &first_key_part[keypart_idx], uchar_key,
                       QUICK_RANGE_flag, range_json);

    uchar_key+= first_key_part[keypart_idx].store_length;
  }
}

void serializeKeyRangeToJson(const key_range *min_key,
                             const key_range *max_key, KEY *key,
                             VidexJsonItem *req_json)
{
  String range_info;
  range_info.set_charset(system_charset_info);

  VidexJsonItem *min_json= req_json->create("min_key");
  subha_parse_key_range(min_key, key, &range_info, min_json);
  std::string std_info_min(range_info.ptr(), range_info.length());
  range_info.length(0);

  VidexJsonItem *max_json= req_json->create("max_key");
  subha_parse_key_range(max_key, key, &range_info, max_json);
  std::string std_info_max(range_info.ptr(), range_info.length());
  range_info.length(0);

  std::stringstream ss;
  ss << "KEY: " << key->name.str << "   MIN_KEY: {" << std_info_min
     << "}, MAX_KEY: {" << std_info_max << "}";
  DBUG_PRINT("info", ("%s", ss.str().c_str()));
  DBUG_PRINT("info", ("req_json = %s", req_json->to_json().c_str()));
}
