#ifndef SQL_TYPE_UUID_INCLUDED
#define SQL_TYPE_UUID_INCLUDED

/* Copyright (c) 2019,2021 MariaDB Corporation

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

#include "sql_type_fixedbin_storage.h"
class UUID: public FixedBinTypeStorage<MY_UUID_SIZE, MY_UUID_STRING_LENGTH>
{
public:
  using FixedBinTypeStorage::FixedBinTypeStorage;
  bool ascii_to_fbt(const char *str, size_t str_length);
  size_t to_string(char *dst, size_t dstsize) const;
  static const Name &default_value();
};

#include "sql_type_fixedbin.h"
typedef FixedBinTypeBundle<UUID> UUIDBundle;

#endif // SQL_TYPE_UUID_INCLUDED
