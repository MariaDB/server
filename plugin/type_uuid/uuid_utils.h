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

#include "mariadb.h"
#include <string>

#define ER_INCORRECT_UUID_VALUE 1
#define ER_INCORRECT_UUID_VARIANT 2 


struct version_and_variant
{
    int version;
    int variant;
    int error_code;
};

bool is_valid_uuid_format_any(const std::string &str);
version_and_variant return_version(const std::string &str);