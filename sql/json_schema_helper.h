#ifndef JSON_SCHEMA_HELPER
#define JSON_SCHEMA_HELPER

/* Copyright (c) 2016, 2021, MariaDB

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA */

#include "sql_type_json.h"
#include <m_string.h>
#include "json_schema.h"

bool json_key_equals(const char* key,  LEX_CSTRING val, int key_len);

bool json_assign_type(uint *curr_type, json_engine_t *je);
uchar* get_key_name(const char *key_name, size_t *length,
                    my_bool /* unused */);
void json_get_normalized_string(json_engine_t *je, String *res,
                                int *error);
#endif
