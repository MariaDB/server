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


void Session_env_dependency::push_dependency_warnings(THD *thd,
                                                      const Item *item) const
{
  sql_mode_t all= m_hard | m_soft;
  for (uint i= 0; all ; i++, all >>= 1)
  {
    if (all & 1)
    {
      switch ((sql_mode_t(1) << i)) {
      case SYS_VAR_TIME_ZONE_TIME_TO_GMT_SEC:
        push_warning_printf(thd,
            Sql_condition::WARN_LEVEL_WARN, ER_UNKNOWN_ERROR,
            "DATETIME to TIMESTAMP conversion depends on @@time_zone");
            //" For safe conversion use: datetime_expr AT TIME ZONE time_zone");
        break;

      case SYS_VAR_TIME_ZONE_GMT_SEC_TO_TIME:
        push_warning_printf(thd,
            Sql_condition::WARN_LEVEL_WARN, ER_UNKNOWN_ERROR,
            "TIMESTAMP to DATETIME conversion depends on @@time_zone");
            //" For safe conversion use: TIMESTAMP_TZ(timestamp_expr, time_zone)");
        break;

      case SYS_VAR_DIV_PRECISION_INCREMENT:
        push_warning_printf(thd,
                            Sql_condition::WARN_LEVEL_WARN, ER_UNKNOWN_ERROR,
                            "Expression depends on @@div_precision_increment");
        break;
      case SYS_VAR_MAX_ALLOWED_PACKET:
        if (!(m_soft & (1 << i)))
          push_warning_printf(thd,
                              Sql_condition::WARN_LEVEL_WARN, ER_UNKNOWN_ERROR,
                              "Its intermediate result "
                              "may be limited by @@max_allowed_packet",
                              item->max_length);
        else
          push_warning_printf(thd,
                              Sql_condition::WARN_LEVEL_WARN, ER_UNKNOWN_ERROR,
                              "The result octet length %u "
                              "may be limited by @@max_allowed_packet",
                              item->max_length);
        break;
      }
    }
  }
}
