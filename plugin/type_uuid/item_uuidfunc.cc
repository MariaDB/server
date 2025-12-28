/* Copyright (c) 2019,2024, MariaDB Corporation

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

String *Item_func_sys_guid::val_str(String *str)
{
  DBUG_ASSERT(fixed());
  str->alloc(uuid_len()+1);
  str->length(uuid_len());
  str->set_charset(collation.collation);

  uchar buf[MY_UUID_SIZE];
  my_uuid(buf);
  my_uuid2str(buf, const_cast<char*>(str->ptr()), 0);
  return str;
}


bool Item_func_uuid_timestamp::fix_length_and_dec(THD *thd)
{
  Type_std_attributes::set(
    Type_temporal_attributes_not_fixed_dec(MAX_DATETIME_WIDTH,
                                           TIME_SECOND_PART_DIGITS, false),
    DTCollation_numeric());
  set_maybe_null();
  return false;
}


bool Item_func_uuid_timestamp::get_timestamp(my_time_t *sec, ulong *usec)
{
  Type_handler_uuid_new::Fbt_null uuid(args[0]);
  if (uuid.is_null())
    return true;
  return my_uuid_extract_ts(uuid.to_lex_cstring().str, sec, usec);
}


bool Item_func_uuid_timestamp::val_native(THD *thd, Native *to)
{
  my_time_t seconds;
  ulong usec;
  return (null_value= get_timestamp(&seconds, &usec)) ||
         (null_value= Timestamp(seconds, usec).to_native(to, TIME_SECOND_PART_DIGITS));
}
