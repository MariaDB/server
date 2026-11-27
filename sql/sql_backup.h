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

#include "sql_cmd.h"


/** BACKUP SERVER */
class Sql_cmd_backup : public Sql_cmd
{
public:
  enum enum_binlog_option {
    BINLOG_FULL, BINLOG_MINIMAL, BINLOG_NONE
  };

private:
  /** target directory, or nullptr when streaming */
  const char *const target{nullptr};
  /** argument of my_popen() for streaming backup, or nullptr */
  const char *const command{nullptr};
  /** number of concurrent threads to use */
  int threads{1};
  /** option for backing up binlog files */
  enum_binlog_option m_binlog_option{BINLOG_FULL};

public:
  /**
     Constructor.
     @param target   name of target or scratch directory
  */
  Sql_cmd_backup(LEX_CSTRING target) : target(target.str) {}
  /**
     Constructor.
     @param command  a shell command for handling a backup stream
  */
  Sql_cmd_backup(LEX_CSTRING command, bool) : command(command.str) {}
  Sql_cmd_backup()= default;
  ~Sql_cmd_backup()= default;

  bool execute(THD *thd) override;

  enum_sql_command sql_command_code() const override
  {
    return SQLCOM_BACKUP_SERVER;
  }
  void concurrent(int val) { threads= val; }
  void binlog_option(enum_binlog_option val) { m_binlog_option= val; }
};
