#ifndef SQL_TYPE_UUID_V1_INCLUDED
#define SQL_TYPE_UUID_V1_INCLUDED

/* Copyright (c) 2024, MariaDB Corporation

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1335  USA */

#include "sql_type_uuid.h"

class UUIDv1: public Type_handler_uuid_new::Fbt
{
public:
  UUIDv1()
  {
    my_uuid((uchar*) m_buffer);
  }
  static bool construct_native(Native *to)
  {
    to->alloc(MY_UUID_SIZE);
    to->length(MY_UUID_SIZE);
    my_uuid((uchar*)to->ptr());
    return 0;
  }
};

#endif // SQL_TYPE_UUID_V1_INCLUDED
