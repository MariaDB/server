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

/*
  RFC 9562 Section 5.1 says UUIDv1 has "a 60-bit timestamp represented
  by Coordinated Universal Time (UTC) as a count of 100-nanosecond
  intervals since 00:00:00.00, 15 October 1582".
  RFC 9562 Section 5.7 Figure 11 says UUIDv7 "unix_ts_ms" is a
  "48-bit big-endian unsigned number of the Unix Epoch timestamp in
  milliseconds".
  MariaDB supports up to microsecond precision for temporal values, so
  UUID_TIMESTAMP() exposes a fixed TIMESTAMP(6) result that can hold the
  most precise supported value.
*/
static constexpr uint UUID_TIMESTAMP_PRECISION= 6;

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
                                           UUID_TIMESTAMP_PRECISION, false),
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

  if ((null_value= get_timestamp(&seconds, &usec)))
    return true;

  Timestamp ts(seconds, usec);
  return (null_value=
          ts.trunc(UUID_TIMESTAMP_PRECISION).to_native(to,
                                                       UUID_TIMESTAMP_PRECISION));
}
