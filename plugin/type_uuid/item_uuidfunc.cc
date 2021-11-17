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

String *Item_func_sys_guid::val_str(String *str)
{
  DBUG_ASSERT(fixed());
  str->alloc(uuid_len()+1);
  str->length(uuid_len());
  str->set_charset(collation.collation);

  uchar buf[MY_UUID_SIZE];
  my_uuid(buf);
  my_uuid2str(buf, const_cast<char*>(str->ptr()), with_dashes);
  return str;
}

const Type_handler *Item_func_uuid::type_handler() const
{
  return UUIDBundle::type_handler_fbt();
}

bool Item_func_uuid::val_native(THD *, Native *to)
{
  DBUG_ASSERT(fixed());
  to->alloc(MY_UUID_SIZE);
  to->length(MY_UUID_SIZE);
  my_uuid((uchar*)to->ptr());
  return 0;
}
