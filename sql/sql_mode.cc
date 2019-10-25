/*
   Copyright (c) 2019, MariaDB.

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
#include "set_var.h"

void Sql_mode_dependency::push_dependency_warnings(THD *thd) const
{
  sql_mode_t all= m_hard | m_soft;
  for (uint i= 0; all ; i++, all >>= 1)
  {
    if (all & 1)
    {
      // TODO-10.5: add a new error code
      push_warning_printf(thd,
                          Sql_condition::WARN_LEVEL_WARN, ER_UNKNOWN_ERROR,
                          "Expression depends on the @@%s value %s",
                          "sql_mode", sql_mode_string_representation(i));
    }
  }
}
