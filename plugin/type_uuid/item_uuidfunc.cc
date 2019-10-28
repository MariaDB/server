/* Copyright (c) 2019,2021, MariaDB Corporation

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

#define MYSQL_SERVER
#include "mariadb.h"
#include "item_uuidfunc.h"
#include "sql_type_uuid.h"

class UUID_generated : public UUIDBundle::Fbt
{
public:
  UUID_generated() { my_uuid((uchar *) m_buffer); }
  bool to_string(String *to, bool with_separators) const
  {
    if (to->alloc(max_char_length() + 1))
      return true;
    to->set_charset(system_charset_info);
    to->length(MY_UUID_BARE_STRING_LENGTH + with_separators*MY_UUID_SEPARATORS);
    my_uuid2str((const uchar *) m_buffer, (char *) to->ptr(), with_separators);
    return false;
  }
};

String *Item_func_uuid::val_str(String *str)
{
  DBUG_ASSERT(fixed());
  if (!UUID_generated().to_string(str, with_separators))
    return str;
  str->set("", 0, collation.collation);
  return str;
}
