/*
  Copyright (c) 2025 MariaDB

  This program is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation; version 2 of the License.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
  See the GNU General Public License for more details.

  You should have received a copy of the GNU General Public License along
  with this program; if not, write to the Free Software Foundation, Inc.,
  51 Franklin St, Fifth Floor, Boston, MA 02110-1335 USA.
*/

#ifndef RPL_RELAY_LOG_INFO_FILE_H
#define RPL_RELAY_LOG_INFO_FILE_H

#include "rpl_info_file.h"

struct Relay_log_info_file: Info_file
{
  /**
    `@@relay_log_info_file` values in SHOW SLAVE STATUS order
    @{
  */
  String_value<> relay_log_file;
  Int_value<my_off_t> relay_log_pos;
  /// Relay_Master_Log_File (of the event *group*)
  String_value<> read_master_log_file;
  /// Exec_Master_Log_Pos (of the event *group*)
  Int_value<my_off_t> read_master_log_pos;
  /// SQL_Delay
  Int_value<uint32_t> sql_delay;
  /// }@

  inline static const Mem_fn VALUE_LIST[] {
    &Relay_log_info_file::relay_log_file,
    &Relay_log_info_file::relay_log_pos,
    &Relay_log_info_file::read_master_log_file,
    &Relay_log_info_file::read_master_log_pos,
    &Relay_log_info_file::sql_delay
  };

  bool load_from_file() override
  {
    return Info_file::load_from_file(VALUE_LIST, /* Exec_Master_Log_Pos */ 4);
  }
  void save_to_file() override
  {
    return Info_file::save_to_file(VALUE_LIST);
  }
};

#endif
