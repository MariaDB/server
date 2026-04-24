/*****************************************************************************
Copyright (c) 2026 MariaDB plc.

This program is free software; you can redistribute it and/or modify it under
the terms of the GNU General Public License as published by the Free Software
Foundation; version 2 of the License.

This program is distributed in the hope that it will be useful, but WITHOUT
ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License along with
this program; if not, write to the Free Software Foundation, Inc.,
51 Franklin Street, Fifth Floor, Boston, MA 02110-1335 USA

*****************************************************************************/

#pragma once

/** BACKUP SERVER */
class Sql_cmd_backup : public Sql_cmd
{
  /** target directory */
  const LEX_CSTRING target;

public:
  explicit Sql_cmd_backup(LEX_CSTRING target) : target(target) {}
  ~Sql_cmd_backup() = default;

  bool execute(THD *thd) override;

  enum_sql_command sql_command_code() const override
  {
    return SQLCOM_BACKUP_SERVER;
  }
};
