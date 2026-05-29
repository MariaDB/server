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

/// @note The line count excludes the line-count line.
struct Relay_log_info_file: Info_file
{
  /*
    Buffers for the String_value members. Declared before the String_value
    members so their size is known when passing to members' constructors.
  */
  char relay_log_file_buf[FN_REFLEN];
  char read_master_log_file_buf[FN_REFLEN];

  /*
    @@relay_log_info_file values in SHOW SLAVE STATUS order.
  */
  String_value relay_log_file{"relay_log_file", relay_log_file_buf,
    sizeof(relay_log_file_buf)};
  Ulonglong_value relay_log_pos{"relay_log_pos"};
  /* Relay_Master_Log_File (of the event group). */
    String_value read_master_log_file{"master_log_file",
      read_master_log_file_buf, sizeof(read_master_log_file_buf)};
  /* Exec_Master_Log_Pos (of the event group). */
    Ulonglong_value read_master_log_pos{"read_master_log_pos"};
  /* SQL_Delay. */
  Uint_value sql_delay{"sql_delay"};

  /*
    Per-instance list of value subobjects, in file order. Must be declared
    after the value members so the brace-init &member references are valid
    when value_list's initialization runs.
  */
  Value *const value_list[5]= {
    &relay_log_file,
    &relay_log_pos,
    &read_master_log_file,
    &read_master_log_pos,
    &sql_delay
  };

  bool load_from_file() override
  {
    return Info_file::load_from_file(value_list,
                                     array_elements(value_list),
                                     /* Exec_Master_Log_Pos */ 4, 0);
  }
  void save_to_file() override
  {
    Info_file::save_to_file(value_list,
                            array_elements(value_list),
                            array_elements(value_list) + 1);
  }
};

#endif
