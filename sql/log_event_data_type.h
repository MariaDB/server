/* Copyright (c) 2024, MariaDB Corporation.

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

#ifndef LOG_EVENT_DATA_TYPE_H
#define LOG_EVENT_DATA_TYPE_H

class Log_event_data_type
{
public:

  enum {
    CHUNK_SIGNED= 0,
    CHUNK_UNSIGNED= 1,
    CHUNK_DATA_TYPE_NAME= 2
  };

protected:
  LEX_CSTRING m_data_type_name;
  Item_result m_type;
  uint m_charset_number;
  bool m_is_unsigned;

public:

  Log_event_data_type()
   :m_data_type_name({NULL,0}),
    m_type(STRING_RESULT),
    m_charset_number(my_charset_bin.number),
    m_is_unsigned(false)
  { }

  Log_event_data_type(const LEX_CSTRING &data_type_name_arg,
                      Item_result type_arg,
                      uint charset_number_arg,
                      bool is_unsigned_arg)
   :m_data_type_name(data_type_name_arg),
    m_type(type_arg),
    m_charset_number(charset_number_arg),
    m_is_unsigned(is_unsigned_arg)
  { }

  const LEX_CSTRING & data_type_name() const
  {
    return m_data_type_name;
  }
  Item_result type() const
  {
    return m_type;
  }
  uint charset_number() const
  {
    return m_charset_number;
  }
  bool is_unsigned() const
  {
    return m_is_unsigned;
  }

  bool unpack_optional_attributes(const char *str, const char *end);
};

#endif // LOG_EVENT_DATA_TYPE_H
