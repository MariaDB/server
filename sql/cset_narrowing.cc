/*
   Copyright (c) 2023, MariaDB Corporation.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1335  USA */

#include "mariadb.h"
#include "sql_priv.h"
#include "sql_select.h"

Charset_utf8narrow utf8mb3_from_mb4;

bool Utf8_narrow::should_do_narrowing(const THD *thd,
                                      CHARSET_INFO *field_cset,
                                      CHARSET_INFO *value_cset)
{
  return optimizer_flag(thd, OPTIMIZER_SWITCH_CSET_NARROWING) &&
           field_cset == &my_charset_utf8mb3_general_ci &&
           value_cset == &my_charset_utf8mb4_general_ci;
}

