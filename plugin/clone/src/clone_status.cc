/* Copyright (c) 2019, 2024, Oracle and/or its affiliates.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License, version 2.0,
   as published by the Free Software Foundation.

   This program is designed to work with certain software (including
   but not limited to OpenSSL) that is licensed under separate terms,
   as designated in a particular file or component or in included license
   documentation.  The authors of MySQL hereby grant you an additional
   permission to link the program and your derivative works with the
   separately licensed software that they have either included with
   the program or referenced in the documentation.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License, version 2.0, for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA */

/**
@file clone/src/clone_status.cc
Clone Plugin: Clone status as performance schema plugin table

*/

#include "clone_status.h"
#include <fstream>
#include <sstream>
#include <string>
// #include "my_io.h"
#include "clone.h"
#include "clone_client.h"

#define SERVICE_TYPE_NO_CONST(X) void

SERVICE_TYPE_NO_CONST(pfs_plugin_table_v1) *mysql_pfs_table = nullptr;
SERVICE_TYPE_NO_CONST(pfs_plugin_column_integer_v1) *mysql_pfscol_int = nullptr;
SERVICE_TYPE_NO_CONST(pfs_plugin_column_bigint_v1) *mysql_pfscol_bigint =
    nullptr;
SERVICE_TYPE_NO_CONST(pfs_plugin_column_string_v2) *mysql_pfscol_string =
    nullptr;
SERVICE_TYPE_NO_CONST(pfs_plugin_column_timestamp_v2) *mysql_pfscol_timestamp =
    nullptr;
SERVICE_TYPE_NO_CONST(pfs_plugin_column_text_v1) *mysql_pfscol_text = nullptr;

#define FILE_PREFIX "#"

/** Clone directory */
#define CLONE_FILES_DIR FILE_PREFIX "clone" FN_DIRSEP

/** Clone recovery status. */
const char CLONE_RECOVERY_FILE[] =
    CLONE_FILES_DIR FILE_PREFIX "status_recovery";

/** Clone PFS view clone_status persister file */
const char CLONE_VIEW_STATUS_FILE[] = CLONE_FILES_DIR FILE_PREFIX "view_status";

/** Clone PFS view clone_progress persister file */
const char CLONE_VIEW_PROGRESS_FILE[] =
    CLONE_FILES_DIR FILE_PREFIX "view_progress";

/* Namespace for all clone data types */
namespace myclone
{
/* PFS proxy table for clone status. */
Status_pfs g_status_table = {};

/* PFS proxy table for clone progress. */
Progress_pfs g_progress_table = {};

/** PFS proxy table array. */
// static PFS_engine_table_share_proxy *pfs_proxy_tables[2] = {nullptr, nullptr};

/** All CLONE state names. */
std::array<const char *, Table_pfs::NUM_STATES> Table_pfs::s_state_names = {};

/** All CLONE stage names. */
std::array<const char *, Table_pfs::NUM_STAGES> Table_pfs::s_stage_names = {};

/** Clone client status data. */
Status_pfs::Data Client::s_status_data = {};

/** Clone client progress data. */
Progress_pfs::Data Client::s_progress_data = {};

/** Mutex to protect status and progress data. */
mysql_mutex_t Client::s_table_mutex;

/** Number of concurrent clone clients. */
uint32_t Client::s_num_clones = 0;

int Table_pfs::create_proxy_tables()
{
  // auto thd = current_thd;
  // if (mysql_pfs_table == nullptr || thd == nullptr)
  // if (!thd)
  //   return 1;

  Client::init_pfs();
  // pfs_proxy_tables[0] = g_status_table.get_proxy_share();
  // pfs_proxy_tables[1] = g_progress_table.get_proxy_share();
  return 0; // mysql_pfs_table->add_tables(pfs_proxy_tables, 2);
}

void Table_pfs::drop_proxy_tables()
{
  // if (mysql_pfs_table != nullptr)
  //   return;

  // static_cast<void>(mysql_pfs_table->delete_tables(pfs_proxy_tables, 2));
  Client::uninit_pfs();
}

bool Table_pfs::acquire_services()
{
  /* Get Table service. */
  // ACQUIRE_SERVICE(mysql_pfs_table, "pfs_plugin_table_v1")
  /* Get column services. */
  // ACQUIRE_SERVICE(mysql_pfscol_int, "pfs_plugin_column_integer_v1")
  // ACQUIRE_SERVICE(mysql_pfscol_bigint, "pfs_plugin_column_bigint_v1")
  // ACQUIRE_SERVICE(mysql_pfscol_string, "pfs_plugin_column_string_v2")
  // ACQUIRE_SERVICE(mysql_pfscol_timestamp, "pfs_plugin_column_timestamp_v2")
  // ACQUIRE_SERVICE(mysql_pfscol_text, "pfs_plugin_column_text_v1")

  auto err = create_proxy_tables();
  if (err != 0)
    return true;

  init_state_names();
  return false;
}

void Table_pfs::init_state_names()
{
  /* Initialise state names. Defaults to nullptr. */
  uint32_t index = 0;
  for (auto &state_name : s_state_names) {
    auto state_index = static_cast<Clone_state>(index);
    switch (state_index)
    {
      case STATE_NONE:
        state_name = "Not Started";
        break;
      case STATE_STARTED:
        state_name = "In Progress";
        break;
      case STATE_SUCCESS:
        state_name = "Completed";
        break;
      case STATE_FAILED:
        state_name = "Failed";
        break;
      default:
        assert(false);
    }
    ++index;
  }
  /* Initialise stage names. Defaults to nullptr. */
  index = 0;
  for (auto &stage_name : s_stage_names)
  {
    auto stage_index = static_cast<Clone_stage>(index);
    switch (stage_index) {
      case STAGE_NONE:
        stage_name = "None";
        break;
      case STAGE_CLEANUP:
        stage_name = "DROP DATA";
        break;
      case STAGE_FILE_COPY:
        stage_name = "FILE COPY";
        break;
      case STAGE_PAGE_COPY:
        stage_name = "PAGE COPY";
        break;
      case STAGE_REDO_COPY:
        stage_name = "REDO COPY";
        break;
      case STAGE_FILE_SYNC:
        stage_name = "FILE SYNC";
        break;
      case STAGE_RESTART:
        stage_name = "RESTART";
        break;
      case STAGE_RECOVERY:
        stage_name = "RECOVERY";
        break;
      default:
        assert(false);
    }
    ++index;
  }
}

void Table_pfs::release_services()
{
  drop_proxy_tables();
}

Table_pfs::Table_pfs(uint32_t num_rows)
    : m_rows(num_rows), m_position(), m_empty(true)
{
}

Status_pfs::Status_pfs() : Table_pfs(S_NUM_ROWS)
{
}

int Status_pfs::rnd_init()
{
  Client::copy_pfs_data(m_data);
  Table_pfs::init_position(m_data.m_id);
  return 0;
}

void Status_pfs::Data::write(bool write_error)
{
  std::string file_name;
  /* Append data directory if cloning to different place. */
  if (!is_local())
  {
    file_name.assign(m_destination);
    file_name.append(FN_DIRSEP);
    file_name.append(CLONE_VIEW_STATUS_FILE);
  }
  else
    file_name.assign(CLONE_VIEW_STATUS_FILE);

  std::ofstream status_file;
  status_file.open(file_name, std::ofstream::out | std::ofstream::trunc);
  if (!status_file.is_open())
    return;

  auto state = static_cast<uint32_t>(m_state);
  /* Write state columns. */
  status_file << state << " " << m_id << std::endl;

  /* Write time columns. */
  status_file << m_start_time << " " << m_end_time << std::endl;

  /* Write source string. */
  status_file << m_source << std::endl;

  /* Write error columns. */
  if (write_error)
  {
    status_file << m_error_number << std::endl;
    status_file << m_error_mesg << std::endl;
  }
  else
  {
    /* Write interrupt error, for possible crash. */
    status_file << ER_QUERY_INTERRUPTED << std::endl;
    status_file << "Query execution was interrupted" << std::endl;
  }
  /* Write binary log information. */
  status_file << m_binlog_file << std::endl;
  status_file << m_binlog_pos << std::endl;
  status_file << m_gtid_string << std::endl;
  status_file.close();
}

void Status_pfs::Data::read()
{
  std::string file_name;
  file_name.assign(CLONE_VIEW_STATUS_FILE);

  std::ifstream status_file;
  status_file.open(file_name, std::ifstream::in);
  if (!status_file.is_open())
    return;

  /* Set fixed data. */
  m_pid = 0;
  strncpy(m_destination, &g_local_string[0], sizeof(m_destination) - 1);

  std::string file_line;
  int line_number = 0;
  uint32_t state = 0;
  /* loop through the lines and extract status information. */
  while (std::getline(status_file, file_line))
  {
    ++line_number;
    std::stringstream file_data(file_line, std::ifstream::in);
    switch (line_number)
    {
      case 1:
        /* Read state columns. */
        file_data >> state >> m_id;
        m_state = STATE_NONE;
        if (state < static_cast<uint32_t>(NUM_STATES))
          m_state = static_cast<Clone_state>(state);
        break;
      case 2:
        /* Read time columns. */
        file_data >> m_start_time >> m_end_time;
        break;
      case 3:
        /* read source string */
        strncpy(m_source, file_line.c_str(), sizeof(m_source) - 1);
        break;
      case 4:
        /* Read error number. */
        file_data >> m_error_number;
        break;
      case 5:
        /* read error string */
        strncpy(m_error_mesg, file_line.c_str(), sizeof(m_error_mesg) - 1);
        break;
      case 6:
        /* Read binary log file name. */
        strncpy(m_binlog_file, file_line.c_str(), sizeof(m_binlog_file) - 1);
        break;
      case 7:
        /* Read binary log position. */
        file_data >> m_binlog_pos;
        break;
      case 8:
        /* Read GTID_EXECUTED. */
        m_gtid_string.assign(file_data.str());
        break;
      default:
        m_gtid_string.append("\n");
        m_gtid_string.append(file_data.str());
        break;
    }
  }
  status_file.close();
}

void Status_pfs::Data::recover()
{
  const std::string file_name(CLONE_RECOVERY_FILE);
  std::ifstream recovery_file;
  recovery_file.open(file_name, std::ifstream::in);
  if (!recovery_file.is_open())
    return;

  std::string file_line;
  int line_number = 0;
  uint64_t recovery_end_time = 0;
  /* loop through the lines and extract binary log information. */
  while (std::getline(recovery_file, file_line))
  {
    ++line_number;
    std::stringstream rec_data(file_line, std::ifstream::in);
    switch (line_number)
    {
      case 1:
        break;
      case 2:
        rec_data >> recovery_end_time;
        break;
      case 3:
        /* Read binary log file name. */
        strncpy(m_binlog_file, file_line.c_str(), sizeof(m_binlog_file) - 1);
        break;
      case 4:
        /* Read binary log position. */
        rec_data >> m_binlog_pos;
        break;
      case 5:
        /* Read GTID_EXECUTED. */
        m_gtid_string.assign(rec_data.str());
        break;
      default:
        m_gtid_string.append("\n");
        m_gtid_string.append(rec_data.str());
        break;
    }
  }
  recovery_file.close();
  std::remove(CLONE_RECOVERY_FILE);

  if (recovery_end_time == 0)
  {
    m_error_number = ER_INTERNAL_ERROR;
    strncpy(m_error_mesg,
            "Recovery failed. Please Retry Clone. "
            "For details, look into server error log.",
            sizeof(m_error_mesg) - 1);
    m_state = STATE_FAILED;
  }
  else
  {
    /* Recovery finished successfully. Reset state and error. */
    m_state = STATE_SUCCESS;
    m_error_number = 0;
    memset(m_error_mesg, 0, sizeof(m_error_mesg));
  }
  /* Update end time for clone operation. */
  m_end_time = recovery_end_time;

  /* Write back to the file after updating binary log positions. */
  write(true);
}

Progress_pfs::Progress_pfs() : Table_pfs(S_NUM_ROWS)
{
}

int Progress_pfs::rnd_init()
{
  Client::copy_pfs_data(m_data);
  Table_pfs::init_position(m_data.m_id);
  return 0;
}

void Progress_pfs::Data::write(const char *data_dir) {
  std::string file_name;

  if (data_dir != nullptr)
  {
    file_name.assign(data_dir);
    file_name.append(FN_DIRSEP);
    file_name.append(CLONE_VIEW_PROGRESS_FILE);
  } else
    file_name.assign(CLONE_VIEW_PROGRESS_FILE);

  std::ofstream status_file;
  status_file.open(file_name, std::ofstream::out | std::ofstream::trunc);
  if (!status_file.is_open())
    return;
  /* Write elements common to all stages. */
  status_file << m_id << std::endl;

  Clone_stage cur_stage = STAGE_NONE;
  next_stage(cur_stage);

  /* Loop through all stages. */
  while (cur_stage != STAGE_NONE)
  {
    auto cur_index = static_cast<uint32_t>(cur_stage);
    Clone_state state = m_states[cur_index];
    /* Unfinished states are marked failed, to indicate error after crash. */
    if (state == STATE_STARTED)
      state = STATE_FAILED;

    status_file << state << " " << m_threads[cur_index] << " "
                << m_start_time[cur_index] << " " << m_end_time[cur_index]
                << " " << m_estimate[cur_index] << " " << m_complete[cur_index]
                << " " << m_network[cur_index] << std::endl;

    next_stage(cur_stage);
  }
  status_file.close();
}

void Progress_pfs::Data::read()
{
  std::string file_name;
  file_name.assign(CLONE_VIEW_PROGRESS_FILE);

  std::ifstream status_file;
  status_file.open(file_name, std::ifstream::in);
  if (!status_file.is_open()) {
    return;
  }

  bool read_common = false;
  Clone_stage cur_stage = STAGE_NONE;
  next_stage(cur_stage);

  std::string file_line;
  /* loop through the lines and extract status information. */
  while (std::getline(status_file, file_line))
  {
    std::stringstream file_data(file_line, std::ifstream::in);
    /* Read information common to all stages. */
    if (!read_common)
    {
      file_data >> m_id;
      read_common = true;
      continue;
    }
    auto cur_index = static_cast<uint32_t>(cur_stage);
    uint32_t state = 0;
    file_data >> state >> m_threads[cur_index] >> m_start_time[cur_index] >>
        m_end_time[cur_index] >> m_estimate[cur_index] >>
        m_complete[cur_index] >> m_network[cur_index];

    m_states[cur_index] = static_cast<Clone_state>(state);
    next_stage(cur_stage);

    if (cur_stage == STAGE_NONE)
      break;
  }
  status_file.close();

  /* Update recovery status. */
  file_name.assign(CLONE_RECOVERY_FILE);
  status_file.open(file_name, std::ifstream::in);

  if (!status_file.is_open())
    return;

  int line_number = 0;
  /* If recovery end time is not written, recovery is not successful. */
  uint64_t recovery_end_time = 0;

  /* loop through the lines and extract binary log information. */
  while (std::getline(status_file, file_line))
  {
    ++line_number;
    std::stringstream rec_data(file_line, std::ifstream::in);
    switch (line_number)
    {
      case 1:
        /* Read recovery start time. */
        rec_data >> m_start_time[STAGE_RECOVERY];
        /* Handle the case when server crashed after successfully completing
        clone but before updating PFS data. */
        if (m_end_time[STAGE_FILE_SYNC] == 0 ||
            m_states[STAGE_FILE_SYNC] != STATE_SUCCESS)
        {
          m_end_time[STAGE_FILE_SYNC] = m_start_time[STAGE_FILE_SYNC];
          m_states[STAGE_FILE_SYNC] = STATE_SUCCESS;
        }
        /* Set server restart stage data. */
        m_start_time[STAGE_RESTART] = m_end_time[STAGE_FILE_SYNC];
        m_end_time[STAGE_RESTART] = m_start_time[STAGE_RECOVERY];
        m_states[STAGE_RESTART] = STATE_SUCCESS;
        break;
      case 2:
        /* Read recovery end time. */
        rec_data >> recovery_end_time;
        break;
      default:
        break;
    }
    if (line_number >= 2)
      break;
  }
  status_file.close();

  m_end_time[STAGE_RECOVERY] = recovery_end_time;
  m_states[STAGE_RECOVERY] =
      (m_end_time[STAGE_RECOVERY] == 0) ? STATE_FAILED : STATE_SUCCESS;

  /* Write back to the file after updating recovery details. */
  write(nullptr);
}

void log_error(THD *thd, bool is_client, int32_t error,
               const char *message_start)
{
  if (error == 0)
  {
    LogPluginErr(INFORMATION_LEVEL,
                 is_client ? ER_CLONE_CLIENT_TRACE : ER_CLONE_SERVER_TRACE,
                 message_start);
    return;
  }

  uint32_t thd_error = 0;
  const char *error_mesg = nullptr;
  clone_get_error(thd, &thd_error, &error_mesg);
  char info_mesg[256];
  snprintf(info_mesg, 256, "%s: error: %d: %s", message_start, error,
           error_mesg != nullptr ? error_mesg : "");

  LogPluginErr(INFORMATION_LEVEL,
               is_client ? ER_CLONE_CLIENT_TRACE : ER_CLONE_SERVER_TRACE,
               info_mesg);
}

}  // namespace myclone
