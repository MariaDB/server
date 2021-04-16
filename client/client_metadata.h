#ifndef SQL_CLIENT_METADATA_INCLUDED
#define SQL_CLIENT_METADATA_INCLUDED
/*
   Copyright (c) 2020, MariaDB Corporation.

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

#include "sql_string.h"


/*
  Print MYSQL_FIELD metadata in human readable format
*/
class Client_field_metadata
{
  const MYSQL_FIELD *m_field;
public:
  Client_field_metadata(MYSQL_FIELD *field)
   :m_field(field)
  { }
  void print_attr(Binary_string *to,
                  const LEX_CSTRING &name,
                  mariadb_field_attr_t attr,
                  uint orig_to_length) const
  {
    MARIADB_CONST_STRING tmp;
    if (!mariadb_field_attr(&tmp, m_field, attr) && tmp.length)
    {
      if (to->length() != orig_to_length)
        to->append(" ", 1);
      to->append(name);
      to->append(tmp.str, tmp.length);
    }
  }
  void print_data_type_related_attributes(Binary_string *to) const
  {
    static const LEX_CSTRING type= {C_STRING_WITH_LEN("type=")};
    static const LEX_CSTRING format= {C_STRING_WITH_LEN("format=")};
    uint to_length_orig= to->length();
    print_attr(to, type, MARIADB_FIELD_ATTR_DATA_TYPE_NAME, to_length_orig);
    print_attr(to, format, MARIADB_FIELD_ATTR_FORMAT_NAME, to_length_orig);
  }
};


#endif // SQL_CLIENT_METADATA_INCLUDED
