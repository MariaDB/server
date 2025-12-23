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
#include <myisampack.h>

// 100-nanosecond intervals between 1582-10-15 and 1970-01-01
#define UUID_TIME_OFFSET ((ulonglong) 141427 * 24 * 60 * 60 * 1000 * 1000 * 10)

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
  return FALSE;
}


bool Item_func_uuid_timestamp::val_native(THD *thd, Native *to)
{
  Type_handler_uuid_new::Fbt_null uuid(args[0]);
  if (uuid.is_null())
    return (null_value= true);

  const uchar *buf= (const uchar *) uuid.to_lex_cstring().str;
  uint version= buf[6] >> 4;

  my_time_t seconds;
  ulong microseconds;

  if (version == 7)
  {
    ulonglong unix_ts_ms= mi_uint6korr(buf);
    seconds= unix_ts_ms / 1000;
    microseconds= (unix_ts_ms % 1000) * 1000;
  }
  else if (version == 1)
  {
    ulonglong uuid_ts= ((ulonglong)(mi_uint2korr(buf + 6) & 0x0FFF) << 48) |
                       ((ulonglong) mi_uint2korr(buf + 4) << 32) |
                        (ulonglong) mi_uint4korr(buf);

    if (uuid_ts < UUID_TIME_OFFSET)
      return (null_value= true);

    ulonglong unix_us= (uuid_ts - UUID_TIME_OFFSET) / 10;
    seconds= unix_us / 1000000;
    microseconds= unix_us % 1000000;
  }
  else
    return (null_value= true);

  return null_value= Timestamp(Timeval(seconds, microseconds)).
                       to_native(to, TIME_SECOND_PART_DIGITS);
}
