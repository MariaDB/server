#ifndef SQL_TYPE_INET_H
#define SQL_TYPE_INET_H
/* Copyright (c) 2011,2013, Oracle and/or its affiliates.
   Copyright (c) 2014 MariaDB Foundation
   Copyright (c) 2019,2021 MariaDB Corporation

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


static const size_t IN_ADDR_SIZE= 4;
static const size_t IN_ADDR_MAX_CHAR_LENGTH= 15;

static const size_t IN6_ADDR_SIZE= 16;
static const size_t IN6_ADDR_NUM_WORDS= IN6_ADDR_SIZE / 2;

/**
  Non-abbreviated syntax is 8 groups, up to 4 digits each,
  plus 7 delimiters between the groups.
  Abbreviated syntax is even shorter.
*/
static const uint IN6_ADDR_MAX_CHAR_LENGTH= 8 * 4 + 7;

#include "sql_type_fixedbin_storage.h"

class Inet6: public FixedBinTypeStorage<IN6_ADDR_SIZE, IN6_ADDR_MAX_CHAR_LENGTH>
{
public:
  using FixedBinTypeStorage::FixedBinTypeStorage;
  bool ascii_to_fbt(const char *str, size_t str_length);
  size_t to_string(char *dst, size_t dstsize) const;
  bool to_bool() const
  {
    return !only_zero_bytes(m_buffer, sizeof(m_buffer));
  }
  static const Name &default_value();
};


#include "sql_type_fixedbin.h"
typedef Type_handler_fbt<Inet6> Type_handler_inet6;

/***********************************************************************/

class Inet4: public FixedBinTypeStorage<IN_ADDR_SIZE, IN_ADDR_MAX_CHAR_LENGTH>
{
public:
  using FixedBinTypeStorage::FixedBinTypeStorage;
  bool ascii_to_fbt(const char *str, size_t str_length);
  size_t to_string(char *dst, size_t dstsize) const;
  static const Name &default_value();
};

typedef Type_handler_fbt<Inet4> Type_handler_inet4;


#endif /* SQL_TYPE_INET_H */
