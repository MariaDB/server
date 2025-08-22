/*****************************************************************************

Copyright (c) 2017, 2024, Oracle and/or its affiliates.

This program is free software; you can redistribute it and/or modify it under
the terms of the GNU General Public License, version 2.0, as published by the
Free Software Foundation.

This program is designed to work with certain software (including
but not limited to OpenSSL) that is licensed under separate terms,
as designated in a particular file or component or in included license
documentation.  The authors of MySQL hereby grant you an additional
permission to link the program and your derivative works with the
separately licensed software that they have either included with
the program or referenced in the documentation.

This program is distributed in the hope that it will be useful, but WITHOUT
ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
FOR A PARTICULAR PURPOSE. See the GNU General Public License, version 2.0,
for more details.

You should have received a copy of the GNU General Public License along with
this program; if not, write to the Free Software Foundation, Inc.,
51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA

*****************************************************************************/

/** @file clone/clone0api.cc
 Innodb Clone Interface

 *******************************************************/
#include <cstdio>
#include <fstream>
#include <iostream>

#define MYSQL_SERVER 1
#include "my_global.h"
#include "sql_class.h"

#include "mysqld.h"
#include "backup.h"
#include "span.h"
#include "sql_table.h"
#include "strfunc.h"
#include "ha_innodb.h"

#include "btr0pcur.h"
#include "clone0api.h"
#include "clone0clone.h"
#include "clone_handler.h"
#include "dict0load.h"
#include "trx0sys.h"

constexpr space_id_t SPACE_UNKNOWN = std::numeric_limits<space_id_t>::max();

extern void ignore_db_dirs_append(const char *dirname_arg);

/** Check if clone status file exists.
@param[in]      file_name       file name
@return true if file exists. */
static bool file_exists(const std::string &file_name)
{
  std::ifstream file(file_name.c_str());

  if (file.is_open())
  {
    file.close();
    return true;
  }
  return false;
}

/** Rename clone status file. The operation is expected to be atomic
when the files belong to same directory.
@param[in]      from_file       name of current file
@param[in]      to_file         name of new file */
static void rename_file(const std::string &from_file,
                        const std::string &to_file)
{
  auto ret= std::rename(from_file.c_str(), to_file.c_str());

  if (ret != 0)
  {
    ib::fatal()
        << "Error renaming file from: " << from_file.c_str()
        << " to: " << to_file.c_str();
  }
}

/** Create clone status file.
@param[in]      file_name       file name */
static void create_file(std::string &file_name)
{
  std::ofstream file(file_name.c_str());

  if (file.is_open())
  {
    file.close();
    return;
  }
  ib::error() << "Error creating file : " << file_name.c_str();
}

/** Delete clone status file or directory.
@param[in]      file    name of file */
static void remove_file(const std::string &file)
{
  os_file_type_t file_type;
  bool exists;

  if (!os_file_status(file.c_str(), &exists, &file_type))
  {
    ib::error() << "Error checking a file to remove : " << file.c_str();
    return;
  }
  /* Allow non existent file, as the server could have crashed or returned
  with error before creating the file. This is needed during error cleanup. */
  if (!exists)
    return;

  /* In C++17 there will be std::filesystem::remove_all and the
  code below will no longer be required. */
  if (file_type == OS_FILE_TYPE_DIR)
  {
    auto scan_cbk= [](const char *path, const char *file_name)
    {
      if (strcmp(file_name, ".") == 0 || strcmp(file_name, "..") == 0)
        return;

      const auto to_remove= std::string{path} + OS_PATH_SEPARATOR + file_name;
      remove_file(to_remove);
    };

    if (!os_file_scan_directory(file.c_str(), scan_cbk, true))
      ib::error() << "Error removing directory : " << file.c_str();
  }
  else
  {
    auto ret= std::remove(file.c_str());
    if (ret != 0)
      ib::error() << "Error removing file : " << file.c_str();
  }
}

/** Create clone in progress file and error file.
@param[in]      clone   clone handle */
static void create_status_file(const Clone_Handle *clone)
{
  const char *path= clone->get_datadir();
  std::string file_name;

  if (clone->replace_datadir())
  {
    /* Create error file for rollback. */
    file_name.assign(CLONE_INNODB_ERROR_FILE);
    create_file(file_name);
    return;
  }

  file_name.assign(path);
  /* Add path separator if needed. */
  if (file_name.back() != OS_PATH_SEPARATOR)
    file_name.append(OS_PATH_SEPARATOR_STR);

  file_name.append(CLONE_INNODB_IN_PROGRESS_FILE);
  create_file(file_name);
}

/** Drop clone in progress file and error file.
@param[in]      clone   clone handle */
static void drop_status_file(const Clone_Handle *clone)
{
  const char *path= clone->get_datadir();
  std::string file_name;

  if (clone->replace_datadir())
  {
    /* Indicate that clone needs table fix up on recovery. */
    file_name.assign(CLONE_INNODB_FIXUP_FILE);
    create_file(file_name);

    /* drop error file on success. */
    file_name.assign(CLONE_INNODB_ERROR_FILE);
    remove_file(file_name);

    DBUG_EXECUTE_IF("clone_recovery_crash_point",
    {
      file_name.assign(CLONE_INNODB_RECOVERY_CRASH_POINT);
      create_file(file_name);
    });
    return;
  }

  std::string path_name(path);
  /* Add path separator if needed. */
  if (path_name.back() != OS_PATH_SEPARATOR)
    path_name.append(OS_PATH_SEPARATOR_STR);

  /* Indicate that clone needs table fix up on recovery. */
  file_name.assign(path_name);
  file_name.append(CLONE_INNODB_FIXUP_FILE);
  create_file(file_name);

  /* Indicate clone needs to update recovery status. */
  file_name.assign(path_name);
  file_name.append(CLONE_INNODB_REPLACED_FILES);
  create_file(file_name);

  /* Mark successful clone operation. */
  file_name.assign(path_name);
  file_name.append(CLONE_INNODB_IN_PROGRESS_FILE);
  remove_file(file_name);
}

void clone_init_list_files()
{
  /* Remove any existing list files. */
  std::string new_files(CLONE_INNODB_NEW_FILES);
  remove_file(new_files);

  std::string old_files(CLONE_INNODB_OLD_FILES);
  remove_file(old_files);

  std::string replaced_files(CLONE_INNODB_REPLACED_FILES);
  remove_file(replaced_files);

  std::string recovery_file(CLONE_INNODB_RECOVERY_FILE);
  remove_file(recovery_file);

  std::string ddl_file(CLONE_INNODB_DDL_FILES);
  remove_file(ddl_file);
}

void clone_remove_list_file(const char *file_name)
{
  std::string list_file(file_name);
  remove_file(list_file);
}

int clone_add_to_list_file(const char *list_file_name, const char *file_name)
{
  std::ofstream list_file;
  list_file.open(list_file_name, std::ofstream::app);

  if (list_file.is_open())
  {
    list_file << file_name << std::endl;

    if (list_file.good())
    {
      list_file.close();
      return 0;
    }
    list_file.close();
  }
  /* This is an error case. Either open or write call failed. */
  char errbuf[MYSYS_STRERROR_SIZE];
  my_error(ER_ERROR_ON_WRITE, MYF(0), list_file_name, errno,
           my_strerror(errbuf, sizeof(errbuf), errno));
  return ER_ERROR_ON_WRITE;
}

/** Add redo log directory to the old file list. */
static void track_redo_files()
{
  const auto path= get_log_file_path();

  /* Skip the path separator which is at the end. */
  ut_ad(!path.empty());
  ut_ad(path.back() == OS_PATH_SEPARATOR);
  const auto str= path.substr(0, path.size() - 1);

  clone_add_to_list_file(CLONE_INNODB_OLD_FILES, str.c_str());
}

/** Execute sql statement.
@param[in,out]  thd             current THD
@param[in]      sql_stmt        SQL statement
@param[in]      thread_number   executing thread number
@param[in]      skip_error      skip statement on error
@return false, if successful. */
// static bool clone_execute_query(THD *thd, const char *sql_stmt,
//                                 size_t thread_number, bool skip_error);

/** Delete all binary logs before clone.
@param[in]      thd     current THD
@return error code */
// static int clone_drop_binary_logs(THD *thd);

/** Drop all user data before starting clone.
@param[in,out]  thd             current THD
@param[in]      allow_threads   allow multiple threads
@return error code */
// static int clone_drop_user_data(THD *thd, bool allow_threads);

/** Open all Innodb tablespaces.
@param[in,out]  thd     session THD
@return error code. */
static int clone_init_tablespaces(THD *thd);

void innodb_clone_get_capability(Ha_clone_flagset &flags)
{
  flags.reset();
  flags.set(HA_CLONE_HYBRID);
  flags.set(HA_CLONE_MULTI_TASK);
  flags.set(HA_CLONE_RESTART);
}

/** Check if clone can be started.
@param[in,out]  thd     session THD
@return error code. */
static int clone_begin_check(THD *thd)
{
  mysql_mutex_assert_owner(clone_sys->get_mutex());
  int err= 0;

  if (Clone_Sys::s_clone_sys_state == CLONE_SYS_ABORT)
    err= ER_CLONE_DDL_IN_PROGRESS;

  if (err != 0 && thd != nullptr)
    my_error(err, MYF(0));

  return err;
}

/** Get clone timeout configuration value.
@param[in,out]  thd             server thread handle
@param[in]      config_name     timeout configuration name
@param[out]     timeout         timeout value
@return true iff successful. */
static bool get_clone_timeout_config(THD *thd, const std::string &config_name,
                                     int &timeout)
{
  timeout= 0;
  using Clone_Key_Values= std::vector<std::pair<std::string, std::string>>;

  /* Get timeout configuration in string format and convert to integer.
  Currently there is no interface to get the integer value directly. The
  variable is in clone plugin and innodb cannot access it directly. */
  Clone_Key_Values timeout_confs= {{config_name, ""}};
  auto err= clone_get_configs(thd, static_cast<void *>(&timeout_confs));

  std::string err_str("Error reading configuration: ");
  err_str.append(config_name);

  if (err != 0)
  {
    ib::error() << err_str;
    return false;
  }

  try
  {
    timeout = std::stoi(timeout_confs[0].second);
  }
  catch (const std::exception &e)
  {
    err_str.append(" Exception: ");
    err_str.append(e.what());
    ib::error() << err_str;
    ut_d(ut_error);
    return false;
  }
  return true;
}

#if 0
/** Timeout while waiting for DDL commands.
@param[in,out]  thd     server thread handle
@return donor timeout in seconds. */
static int get_ddl_timeout(THD *thd)
{
  int timeout= 0;
  std::string config_timeout("clone_ddl_timeout");

  if (!get_clone_timeout_config(thd, config_timeout, timeout))
    /* Default to five minutes in case error reading configuration. */
    timeout = 300;

  return timeout;
}
#endif

int innodb_clone_begin(THD *thd, const byte *&loc, uint &loc_len,
                       uint &task_id, Ha_clone_type type, Ha_clone_mode mode)
{
  /* Check if reference locator is valid */
  if (loc != nullptr && !clone_validate_locator(loc, loc_len))
  {
    int err= ER_CLONE_PROTOCOL;
    my_error(err, MYF(0), "Wrong Clone RPC: Invalid Locator");
    return err;
  }

  /* Acquire clone system mutex which would automatically get released
  when we return from the function [RAII]. */
  Mysql_mutex_guard sys_mutex(clone_sys->get_mutex());

  /* Check if concurrent ddl has marked abort. */
  int err = clone_begin_check(thd);

  if (err != 0)
    return err;

  /* Check of clone is already in progress for the reference locator. */
  auto clone_hdl= clone_sys->find_clone(loc, loc_len, CLONE_HDL_COPY);

  switch (mode)
  {
    case HA_CLONE_MODE_RESTART:
      /* Error out if existing clone is not found */
      if (clone_hdl == nullptr)
      {
        my_error(ER_INTERNAL_ERROR, MYF(0),
                 "Innodb Clone Restart could not find existing clone");
        return (ER_INTERNAL_ERROR);
      }
      ib::info() << "Clone Begin Master Task: Restart";
      err= clone_hdl->restart_copy(thd, loc, loc_len);
      break;

    case HA_CLONE_MODE_START:
    {
      /* Should not find existing clone for the locator */
      if (clone_hdl != nullptr)
      {
        clone_sys->drop_clone(clone_hdl);
        my_error(ER_INTERNAL_ERROR, MYF(0),
                 "Innodb Clone Begin refers existing clone");
        return ER_INTERNAL_ERROR;
      }
      auto &sctx= thd->main_security_ctx;

      /* Should not become a donor when provisioning is started. */
      if (Clone_handler::is_provisioning() && sctx.host_or_ip)
      {
        if (0 == strcmp(my_localhost, sctx.host_or_ip))
        {
          my_error(ER_CLONE_LOOPBACK, MYF(0));
          return ER_CLONE_LOOPBACK;
        }
        my_error(ER_CLONE_TOO_MANY_CONCURRENT_CLONES, MYF(0), MAX_CLONES);
        return ER_CLONE_TOO_MANY_CONCURRENT_CLONES;
      }

      /* Log user and host beginning clone operation. */
      ib::info() << "Clone Begin Master Task by "
                 << sctx.user << "@" << sctx.host_or_ip;
      break;
    }

    case HA_CLONE_MODE_ADD_TASK:
      /* Should find existing clone for the locator */
      if (clone_hdl == nullptr)
      {
        /* Operation has finished already */
        my_error(ER_INTERNAL_ERROR, MYF(0),
                 "Innodb Clone add task refers non-existing clone");

        return ER_INTERNAL_ERROR;
      }
      break;

    case HA_CLONE_MODE_VERSION:
    case HA_CLONE_MODE_MAX:
    default:
      my_error(ER_INTERNAL_ERROR, MYF(0), "Innodb Clone Begin Invalid Mode");

      ut_d(ut_error);
      return ER_INTERNAL_ERROR;
  }

  if (clone_hdl == nullptr)
  {
    ut_ad(thd != nullptr);
    ut_ad(mode == HA_CLONE_MODE_START);

    /* Create new clone handle for copy. Reference locator
    is used for matching the version. */
    auto err= clone_sys->add_clone(loc, CLONE_HDL_COPY, clone_hdl);

    if (err != 0)
      return err;

    err= clone_hdl->init(loc, loc_len, type, nullptr);

    /* Check and wait if clone is marked for wait. */
    if (err == 0)
    {
#if 0
      auto timeout= get_ddl_timeout(thd);
      /* zero timeout is special mode when DDL can abort running clone. */
      if (timeout == 0)
        clone_hdl->set_ddl_abort();
#endif
      err= clone_sys->wait_for_free(thd);
    }

    /* Re-check for initial errors as we could have released sys mutex
    before allocating clone handle. */
    if (err == 0)
      err = clone_begin_check(thd);

    if (err != 0)
    {
      clone_sys->drop_clone(clone_hdl);
      return err;
    }
  }

  /* Add new task for the clone copy operation. */
  if (err == 0)
  {
    /* Release clone system mutex here as we might need to wait while
    adding task. It is safe as the clone handle is acquired and cannot
    be freed till we release it. */
    mysql_mutex_unlock(clone_sys->get_mutex());
    err= clone_hdl->add_task(thd, nullptr, 0, task_id);

    /* Open all tablespaces in Innodb if not done during bootstrap. */
    if (err == 0 && task_id == 0)
      err= clone_init_tablespaces(thd);
    mysql_mutex_lock(clone_sys->get_mutex());
  }

  if (err != 0)
  {
    clone_sys->drop_clone(clone_hdl);
    return err;
  }

  if (task_id > 0)
    ib::info() << "Clone Begin Task ID: " << task_id;

  /* Get the current locator from clone handle. */
  loc= clone_hdl->get_locator(loc_len);
  return 0;
}

int innodb_clone_copy(THD *thd, const byte *loc, uint loc_len, uint task_id,
                      Ha_clone_stage stage, Ha_clone_cbk *cbk)
{
  /* Get clone handle by locator index. */
  auto clone_hdl= clone_sys->get_clone_by_index(loc, loc_len);

  auto err= clone_hdl->check_error(thd);

  ut_ad(stage >= HA_CLONE_STAGE_DDL_BLOCKED);
  if (err != 0)
    return err;

  /* Start data copy. */
  bool post_snapshot= (stage > HA_CLONE_STAGE_SNAPSHOT);
  if (stage == HA_CLONE_STAGE_SNAPSHOT)
    err= clone_hdl->snapshot();
  else
    err= clone_hdl->copy(task_id, cbk, post_snapshot);

  clone_hdl->save_error(err);
  return err;
}

int innodb_clone_ack(THD *thd, const byte *loc, uint loc_len,
                     uint task_id, int in_err, Ha_clone_cbk *cbk)
{
  /* Check if reference locator is valid */
  if (loc != nullptr && !clone_validate_locator(loc, loc_len))
  {
    int err= ER_CLONE_PROTOCOL;
    my_error(err, MYF(0), "Wrong Clone RPC: Invalid Locator");
    return err;
  }
  mysql_mutex_lock(clone_sys->get_mutex());

  /* Find attach clone handle using the reference locator. */
  auto clone_hdl= clone_sys->find_clone(loc, loc_len, CLONE_HDL_COPY);

  mysql_mutex_unlock(clone_sys->get_mutex());

  /* Must find existing clone for the locator */
  if (clone_hdl == nullptr)
  {
    my_error(ER_INTERNAL_ERROR, MYF(0),
             "Innodb Clone ACK refers non-existing clone");
    return ER_INTERNAL_ERROR;
  }
  int err= 0;

  /* If thread is interrupted, then set interrupt error instead. */
  if (thd_killed(thd))
  {
    my_error(ER_QUERY_INTERRUPTED, MYF(0));
    in_err= ER_QUERY_INTERRUPTED;
  }

  if (in_err == 0)
  {
    /* Apply acknowledged data */
    err = clone_hdl->apply(thd, task_id, cbk);
    clone_hdl->save_error(err);
  }
  else
  {
    /* For error input, return after saving it */
    ib::info() << "Clone set error ACK: " << in_err;
    clone_hdl->save_error(in_err);
  }
  mysql_mutex_lock(clone_sys->get_mutex());

  /* Detach from clone handle */
  clone_sys->drop_clone(clone_hdl);

  mysql_mutex_unlock(clone_sys->get_mutex());
  return err;
}

/** Timeout while waiting for recipient after network failure.
@param[in,out]  thd     server thread handle
@return donor timeout in minutes. */
static Clone_Min get_donor_timeout(THD *thd)
{
  int timeout = 0;
  std::string config_timeout("clone_donor_timeout_after_network_failure");

  if (!get_clone_timeout_config(thd, config_timeout, timeout))
    /* Default to five minutes in case error reading configuration. */
    timeout = 5;

  return Clone_Min(timeout);
}

int innodb_clone_end(THD *thd, const byte *loc, uint loc_len,
                     uint task_id, int in_err)
{
  /* Acquire clone system mutex which would automatically get released
  when we return from the function [RAII]. */
  Mysql_mutex_guard sys_mutex(clone_sys->get_mutex());

  /* Get clone handle by locator index. */
  auto clone_hdl= clone_sys->get_clone_by_index(loc, loc_len);

  /* If thread is interrupted, then set interrupt error instead. */
  if (thd_killed(thd))
  {
    my_error(ER_QUERY_INTERRUPTED, MYF(0));
    in_err= ER_QUERY_INTERRUPTED;
  }
  /* Set error, if already not set */
  clone_hdl->save_error(in_err);

  /* Drop current task. */
  bool is_master= false;
  auto wait_reconnect= clone_hdl->drop_task(thd, task_id, is_master);
  auto is_copy= clone_hdl->is_copy_clone();
  auto is_init= clone_hdl->is_init();
  auto is_abort= clone_hdl->is_abort();

  if (!wait_reconnect || is_abort)
  {
    if (is_copy && is_master)
    {
      if (is_abort)
      {
        ib::info() << "Clone Master aborted by concurrent clone";
        clone_hdl->set_abort();
      }
      else if (in_err != 0)
        /* Make sure re-start attempt fails immediately */
        clone_hdl->set_abort();
    }

    if (!is_copy && !is_init && is_master)
    {
      if (in_err == 0)
        /* On success for apply handle, drop status file. */
        drop_status_file(clone_hdl);
      else if (clone_hdl->replace_datadir())
        /* On failure, rollback if replacing current data directory. */
        clone_files_error();
    }
    clone_sys->drop_clone(clone_hdl);

    auto da= thd->get_stmt_da();
    ib::info()
        << "Clone"
        << (is_copy ? " End" : (is_init ? " Apply Version End" : " Apply End"))
        << (is_master ? " Master" : "") << " Task ID: " << task_id
        << (in_err != 0 ? " Failed, code: " : " Passed, code: ") << in_err
        << ": "
        << ((in_err == 0 || da == nullptr || !da->is_error()) ?
           "" : da->message());
    return 0;
  }

  ut_ad(clone_hdl->is_copy_clone());
  ut_ad(is_master);

  auto da= thd->get_stmt_da();
  ib::info()
      << "Clone Master n/w error code: " << in_err << ": "
      << ((da == nullptr || !da->is_error()) ? "" : da->message());

  auto time_out= get_donor_timeout(thd);

  if (time_out.count() <= 0)
  {
    ib::info() << "Clone Master Skip wait after n/w error. Dropping Snapshot.";
    clone_sys->drop_clone(clone_hdl);
    return 0;
  }

  ib::info() << "Clone Master wait " << time_out.count()
             << " minutes for restart after n/w error";

  /* Set state to idle and wait for re-connect */
  clone_hdl->set_state(CLONE_STATE_IDLE);
  /* Sleep for 1 second */
  Clone_Msec sleep_time(Clone_Sec(1));
  /* Generate alert message every minute. */
  Clone_Sec alert_interval(Clone_Min(1));

  /* Wait for client to reconnect back */
  bool is_timeout= false;
  auto err= Clone_Sys::wait(sleep_time, time_out, alert_interval,
                            [&](bool alert, bool &result)
      {
        mysql_mutex_assert_owner(clone_sys->get_mutex());
        result = !clone_hdl->is_active();

        if (thd_killed(thd) || clone_hdl->is_interrupted())
        {
          ib::info()
              << "Clone End Master wait for Restart interrupted";
          my_error(ER_QUERY_INTERRUPTED, MYF(0));
          return ER_QUERY_INTERRUPTED;

        }
        else if (Clone_Sys::s_clone_sys_state == CLONE_SYS_ABORT)
        {
          ib::info()
              << "Clone End Master wait for Restart aborted by DDL";
          my_error(ER_CLONE_DDL_IN_PROGRESS, MYF(0));
          return ER_CLONE_DDL_IN_PROGRESS;

        }
        else if (clone_hdl->is_abort())
        {
          result= false;
          ib::info() << "Clone End Master wait for Restart"
                        " aborted by concurrent clone";
          return 0;
        }

        if (!result)
          ib::info() << "Clone Master restarted successfully by "
                        "other task after n/w failure";
        else if (alert)
          ib::info() << "Clone Master still waiting for restart";

        return 0;
      }, clone_sys->get_mutex(), is_timeout);

  if (err == 0 && is_timeout && clone_hdl->is_idle())
    ib::info() << "Clone End Master wait for restart timed out after "
               << time_out.count() << " minutes. Dropping Snapshot";

  /* If Clone snapshot is not restarted, at this point mark it for
  abort and end the snapshot to allow any waiting DDL to unpin the
  handle and exit. */
  if (!clone_hdl->is_active())
  {
    ut_ad(err != 0 || is_timeout);
    clone_hdl->set_abort();
  }
  /* Last task should drop the clone handle. */
  clone_sys->drop_clone(clone_hdl);
  return 0;
}

int innodb_clone_apply_begin(THD *thd, const byte *&loc,
                             uint &loc_len, uint &task_id, Ha_clone_mode mode,
                             const char *data_dir)
{
  /* Check if reference locator is valid */
  if (loc != nullptr && !clone_validate_locator(loc, loc_len))
  {
    int err= ER_CLONE_PROTOCOL;
    my_error(err, MYF(0), "Wrong Clone RPC: Invalid Locator");
    return err;
  }
  /* Acquire clone system mutex which would automatically get released
  when we return from the function [RAII]. */
  Mysql_mutex_guard sys_mutex(clone_sys->get_mutex());

  /* Check if clone is already in progress for the reference locator. */
  auto clone_hdl= clone_sys->find_clone(loc, loc_len, CLONE_HDL_APPLY);

  switch (mode)
  {
    case HA_CLONE_MODE_RESTART:
    {
      ib::info() << "Clone Apply Begin Master Task: Restart";
      auto err= clone_hdl->restart_apply(thd, loc, loc_len);

      /* Reduce reference count */
      clone_sys->drop_clone(clone_hdl);

      /* Restart is done by master task */
      ut_ad(task_id == 0);
      task_id= 0;

      return err;
    }
    case HA_CLONE_MODE_START:

      if (clone_hdl != nullptr)
      {
        clone_sys->drop_clone(clone_hdl);
        ib::error() << "Clone Apply Begin Master found duplicate clone";
        clone_hdl= nullptr;
        ut_d(ut_error);
      }
      /* Check if the locator is from current mysqld server. */
      clone_hdl= clone_sys->find_clone(loc, loc_len, CLONE_HDL_COPY);

      if (clone_hdl != nullptr)
      {
        clone_sys->drop_clone(clone_hdl);
        clone_hdl= nullptr;
        ib::info() << "Clone Apply Master Loop Back";
        ut_ad(data_dir != nullptr);
      }
      ib::info() << "Clone Apply Begin Master Task";
      break;

    case HA_CLONE_MODE_ADD_TASK:
      /* Should find existing clone for the locator */
      if (clone_hdl == nullptr)
      {
        /* Operation has finished already */
        my_error(ER_INTERNAL_ERROR, MYF(0),
                 "Innodb Clone Apply add task to non-existing clone");
        return ER_INTERNAL_ERROR;
      }
      break;

    case HA_CLONE_MODE_VERSION:
      /* Cannot have input locator or existing clone */
      ib::info() << "Clone Apply Begin Master Version Check";
      ut_ad(loc == nullptr);
      ut_ad(clone_hdl == nullptr);
      break;

    case HA_CLONE_MODE_MAX:
    default:
      my_error(ER_INTERNAL_ERROR, MYF(0),
               "Innodb Clone Appply Begin Invalid Mode");
      ut_d(ut_error);
      return ER_INTERNAL_ERROR;
  }

  if (clone_hdl == nullptr)
  {
    ut_ad(thd != nullptr);

    ut_ad(mode == HA_CLONE_MODE_VERSION || mode == HA_CLONE_MODE_START);

    /* Create new clone handle for apply. Reference locator
    is used for matching the version. */
    auto err= clone_sys->add_clone(loc, CLONE_HDL_APPLY, clone_hdl);
    if (err != 0)
      return (err);

    err= clone_hdl->init(loc, loc_len, HA_CLONE_HYBRID, data_dir);

    if (err != 0)
    {
      clone_sys->drop_clone(clone_hdl);
      return (err);
    }
  }

  if (clone_hdl->is_active())
  {
    /* Release clone system mutex here as we might need to wait while
    adding task. It is safe as the clone handle is acquired and cannot
    be freed till we release it. */
    mysql_mutex_unlock(clone_sys->get_mutex());

    /* Create status file to indicate active clone directory. */
    if (mode == HA_CLONE_MODE_START)
      create_status_file(clone_hdl);

    int err= 0;
    /* Drop any user data after acquiring backup lock. Don't allow
    concurrent threads as the BACKUP MDL lock would not allow any
    other threads to execute DDL. */
    if (clone_hdl->replace_datadir() && mode == HA_CLONE_MODE_START)
    {
      /* Safeguard to throw error if innodb read only mode is on. Currently
      not reachable as we would get error much earlier while dropping user
      tables. */
      if (srv_read_only_mode)
      {
        err= ER_INTERNAL_ERROR;
        my_error(err, MYF(0),
                 "Clone cannot replace data with innodb_read_only = ON");
        ut_d(ut_error);
      }
      else
      {
        track_redo_files();
        /* TODO: err= clone_drop_user_data(thd, false);
        if (err != 0)
          clone_files_error();
        */
      }
    }

    /* Add new task for the clone apply operation. */
    if (err == 0)
    {
      ut_ad(loc != nullptr);
      err= clone_hdl->add_task(thd, loc, loc_len, task_id);
    }
    mysql_mutex_lock(clone_sys->get_mutex());

    if (err != 0)
    {
      clone_sys->drop_clone(clone_hdl);
      return err;
    }
  }
  else
  {
    ut_ad(mode == HA_CLONE_MODE_VERSION);
    /* Set all clone status files empty. */
    if (clone_hdl->replace_datadir())
      clone_init_list_files();
  }

  if (task_id > 0)
    ib::info() << "Clone Apply Begin Task ID: " << task_id;

  /* Get the current locator from clone handle. */
  if (mode != HA_CLONE_MODE_ADD_TASK)
    loc = clone_hdl->get_locator(loc_len);

  return 0;
}

int innodb_clone_apply(THD *thd, const byte *loc,
                       uint loc_len, uint task_id, int in_err,
                       Ha_clone_cbk *cbk)
{
  /* Get clone handle by locator index. */
  auto clone_hdl= clone_sys->get_clone_by_index(loc, loc_len);
  ut_ad(in_err != 0 || cbk != nullptr);

  /* For error input, return after saving it */
  if (in_err != 0 || cbk == nullptr)
  {
    clone_hdl->save_error(in_err);
    auto da= thd->get_stmt_da();
    ib::info()
        << "Clone Apply set error code: " << in_err << ": "
        << ((in_err == 0 || da == nullptr || !da->is_error()) ?
            "" : da->message());
    return 0;
  }

  auto err= clone_hdl->check_error(thd);
  if (err != 0)
    return err;

  /* Apply data received from callback. */
  err= clone_hdl->apply(thd, task_id, cbk);
  clone_hdl->save_error(err);

  return err;
}

int innodb_clone_apply_end(THD *thd, const byte *loc,
                           uint loc_len, uint task_id, int in_err)
{
  auto err= innodb_clone_end(thd, loc, loc_len, task_id, in_err);
  return err;
}

/* Logical bitmap for clone file state. */

/** Data file is found. */
const int FILE_DATA = 1;
/** Saved data file is found */
const int FILE_SAVED = 10;
/** Cloned data file is found */
const int FILE_CLONED = 100;

/** NONE state: file not present. */
const int FILE_STATE_NONE = 0;
/** Normal state: only data file is present. */
const int FILE_STATE_NORMAL = FILE_DATA;
/** Saved state: only saved data file is present. */
const int FILE_STATE_SAVED = FILE_SAVED;
/** Cloned state: data file and cloned data file are present. */
const int FILE_STATE_CLONED = FILE_DATA + FILE_CLONED;
/** Saved clone state: saved data file and cloned data file are present. */
const int FILE_STATE_CLONE_SAVED = FILE_SAVED + FILE_CLONED;
/** Replaced state: saved data file and data file are present. */
const int FILE_STATE_REPLACED = FILE_SAVED + FILE_DATA;

/* Clone data File state transfer.
  [FILE_STATE_NORMAL] --> [FILE_STATE_CLONED]
    Remote data is cloned into another file named <file_name>.clone.

  [FILE_STATE_CLONED] --> [FILE_STATE_CLONE_SAVED]
    Before recovery the datafile is saved in a file named <file_name>.save.

  [FILE_STATE_CLONE_SAVED] --> [FILE_STATE_REPLACED]
    Before recovery the cloned file is moved to datafile.

  [FILE_STATE_REPLACED] --> [FILE_STATE_NORMAL]
    After successful recovery the saved data file is removed.

  Every state transition involves a single file create, delete or rename and
  we consider them atomic. In case of a failure the state rolls back exactly
  in reverse order.
*/

/** Check if a file exists.
@param[in]  path  file path name
@return true if file exists. */
static bool os_file_exists(const char *path)
{
  os_file_type_t type;
  bool exists= false;
  bool ret= os_file_status(path, &exists, &type);

  return ret && exists;
}

/** Get current state of a clone file.
@param[in]      data_file       data file name
@return current file state. */
static int get_file_state(const std::string &data_file)
{
  int state = 0;
  /* Check if data file is there. */
  if (os_file_exists(data_file.c_str()))
    state += FILE_DATA;

  std::string saved_file(data_file);
  saved_file.append(CLONE_INNODB_SAVED_FILE_EXTN);

  /* Check if saved old file is there. */
  if (os_file_exists(saved_file.c_str()))
    state += FILE_SAVED;

  std::string cloned_file(data_file);
  cloned_file.append(CLONE_INNODB_REPLACED_FILE_EXTN);

  /* Check if cloned file is there. */
  if (os_file_exists(cloned_file.c_str()))
    state += FILE_CLONED;

  return state;
}

/** Roll forward clone file state till final state.
@param[in]      data_file       data file name
@param[in]      final_state     data file state to forward to
@return previous file state before roll forward. */
static int file_roll_forward(const std::string &data_file, int final_state)
{
  auto cur_state= get_file_state(data_file);

  switch (cur_state)
  {
    case FILE_STATE_CLONED:
    {
      if (final_state == FILE_STATE_CLONED)
        break;
      /* Save data file */
      std::string saved_file(data_file);
      saved_file.append(CLONE_INNODB_SAVED_FILE_EXTN);
      rename_file(data_file, saved_file);
      ib::info()
          << "Clone File Roll Forward: Save data file " << data_file
          << " state: " << cur_state;
    }
    [[fallthrough]];

    case FILE_STATE_CLONE_SAVED:
    {
      if (final_state == FILE_STATE_CLONE_SAVED)
        break;
      /* Replace data file with cloned file. */
      std::string cloned_file(data_file);
      cloned_file.append(CLONE_INNODB_REPLACED_FILE_EXTN);
      rename_file(cloned_file, data_file);
      ib::info()
          << "Clone File Roll Forward: Rename clone to data file " << data_file
          << " state: " << cur_state;
    }
    [[fallthrough]];

    case FILE_STATE_REPLACED:
    {
      if (final_state == FILE_STATE_REPLACED)
        break;
      /* Remove saved data file */
      std::string saved_file(data_file);
      saved_file.append(CLONE_INNODB_SAVED_FILE_EXTN);
      remove_file(saved_file);
      ib::info()
          << "Clone File Roll Forward: Remove saved data file " << data_file
          << " state: " << cur_state;
    }
    [[fallthrough]];

    case FILE_STATE_NORMAL:
      /* Nothing to do. */
      break;

    default:
      ib::fatal()
          << "Clone File Roll Forward: Invalid File State: " << cur_state;
  }
  return cur_state;
}

/** Roll back clone file state to normal state.
@param[in]      data_file       data file name */
static void file_rollback(const std::string &data_file)
{
  auto cur_state= get_file_state(data_file);
  switch (cur_state)
  {
    case FILE_STATE_REPLACED:
    {
      /* Replace data file back to cloned file. */
      std::string cloned_file(data_file);
      cloned_file.append(CLONE_INNODB_REPLACED_FILE_EXTN);
      rename_file(data_file, cloned_file);
      ib::info()
          << "Clone File Roll Back: Rename data to cloned file " << data_file
          << " state: " << cur_state;
    }
    [[fallthrough]];

    case FILE_STATE_CLONE_SAVED:
    {
      /* Replace data file with saved file. */
      std::string saved_file(data_file);
      saved_file.append(CLONE_INNODB_SAVED_FILE_EXTN);
      rename_file(saved_file, data_file);
      ib::info()
          << "Clone File Roll Back: Rename saved to data file " << data_file
          << " state: " << cur_state;
    }
    [[fallthrough]];

    case FILE_STATE_CLONED:
    {
      /* Remove cloned data file. */
      std::string cloned_file(data_file);
      cloned_file.append(CLONE_INNODB_REPLACED_FILE_EXTN);
      remove_file(cloned_file);
      ib::info()
          << "Clone File Roll Back: Remove cloned file " << data_file
          << " state: " << cur_state;
    }
    [[fallthrough]];

    case FILE_STATE_NORMAL:
      /* Nothing to do. */
      break;

    default:
      ib::fatal()
          << "Clone File Roll Back: Invalid File State: " << cur_state;
  }
}

/* Clone old data File state transfer. These files are present only in
recipient and we haven't drop the database objects (table/tablespace)
before clone. Currently used for user created undo tablespace. Dropping
undo tablespace could be expensive as we need to wait for purge to finish.
  [FILE_STATE_NORMAL] --> [FILE_STATE_SAVED]
    Before recovery the old datafile is saved in a file named <file_name>.save.

  [FILE_STATE_SAVED] --> [FILE_STATE_NONE]
    After successful recovery the saved data file is removed.

  These state transitions involve a single file delete or rename and
  we consider them atomic. In case of a failure the state rolls back.

  [FILE_STATE_SAVED] --> [FILE_STATE_NORMAL]
    On failure saved data file is moved back to original data file.
*/

/** Roll forward old data file state till final state.
@param[in]      data_file       data file name
@param[in]      final_state     data file state to forward to */
static void old_file_roll_forward(const std::string &data_file,
                                  int final_state)
{
  auto cur_state= get_file_state(data_file);

  switch (cur_state)
  {
    case FILE_STATE_CLONED:
    case FILE_STATE_CLONE_SAVED:
    case FILE_STATE_REPLACED:
      /* If the file is also cloned, we can skip here as it would be handled
      with other cloned files. */
      ib::info()
          << "Clone Old File Roll Forward: Skipped cloned file " << data_file
          << " state: " << cur_state;
      break;
    case FILE_STATE_NORMAL:
    {
      if (final_state == FILE_STATE_NORMAL)
      {
        ut_d(ut_error);
        break;
      }
      /* Save data file */
      std::string saved_file(data_file);
      saved_file.append(CLONE_INNODB_SAVED_FILE_EXTN);
      rename_file(data_file, saved_file);
      ib::info()
          << "Clone Old File Roll Forward: Saved data file " << data_file
          << " state: " << cur_state;
    }
    [[fallthrough]];

    case FILE_STATE_SAVED:
    {
      if (final_state == FILE_STATE_SAVED)
        break;
      /* Remove saved data file */
      std::string saved_file(data_file);
      saved_file.append(CLONE_INNODB_SAVED_FILE_EXTN);
      remove_file(saved_file);
      ib::info()
          << "Clone Old File Roll Forward: Remove saved file " << data_file
          << " state: " << cur_state;
    }
    [[fallthrough]];

    case FILE_STATE_NONE:
      /* Nothing to do. */
      break;

    default:
      ib::fatal()
          << "Clone Old File Roll Forward: Invalid File State: " << cur_state;
  }
}

/** Roll back old data file state to normal state.
@param[in]      data_file       data file name */
static void old_file_rollback(const std::string &data_file)
{
  auto cur_state= get_file_state(data_file);

  switch (cur_state)
  {
    case FILE_STATE_CLONED:
    case FILE_STATE_CLONE_SAVED:
    case FILE_STATE_REPLACED:
      /* If the file is also cloned, we can skip here as it would be handled
      with other cloned files. */
      ib::info()
          << "Clone Old File Roll Back: Skip cloned file " << data_file
          << " state: " << cur_state;
      break;

    case FILE_STATE_SAVED:
    {
      /* Replace data file with saved file. */
      std::string saved_file(data_file);
      saved_file.append(CLONE_INNODB_SAVED_FILE_EXTN);
      rename_file(saved_file, data_file);
      ib::info()
          << "Clone Old File Roll Back: Renamed saved data file " << data_file
          << " state: " << cur_state;
    }
    [[fallthrough]];

    case FILE_STATE_NORMAL:
    case FILE_STATE_NONE:
      /* Nothing to do. */
      break;

    default:
      ib::fatal()
          << "Clone Old File Roll Back: Invalid File State: " << cur_state;
  }
}

/** Fatal error callback function. Don't call other functions from here. Don't
use ut_a, ut_ad asserts or ib::fatal to avoid recursive invocation. */
static void clone_files_fatal_error()
{
  /* Safeguard to avoid recursive call. */
  static bool started_error_handling= false;
  if (started_error_handling)
    return;

  started_error_handling= true;

  std::ifstream err_file(CLONE_INNODB_ERROR_FILE);
  if (err_file.is_open())
    err_file.close();
  else
  {
    /* Create error file if not there. */
    std::ofstream new_file(CLONE_INNODB_ERROR_FILE);
    /* On creation failure, return and abort. */
    if (!new_file.is_open())
      return;
    new_file.close();
  }
  /* In case of fatal error, from ib::fatal and ut_a asserts
  we terminate the process here and send the exit status so that a
  managed server can be restarted with older data files. */
  // std::_Exit(MYSQLD_RESTART_EXIT);
}

/** Update recovery status file at end of clone recovery.
@param[in]      finished        true if finishing clone recovery
@param[in]      is_error        if recovery error
@param[in]      is_replace      true, if replacing current directory */
static void clone_update_recovery_status(bool finished, bool is_error,
                                         bool is_replace)
{
  /* true, when we are recovering a cloned database. */
  static bool recovery_in_progress= false;
  /* true, when replacing current data directory. */
  static bool recovery_replace= false;

  std::function<void()> callback_function;

  /* Mark the beginning of clone recovery. */
  if (!finished)
  {
    recovery_in_progress= true;
    if (is_replace)
    {
      recovery_replace= true;
      callback_function= clone_files_fatal_error;
      ut_set_assert_callback(callback_function);
    }
    return;
  }
  is_replace= recovery_replace;
  recovery_replace= false;

  /* Update status only if clone recovery in progress. */
  if (!recovery_in_progress)
    return;

  /* Mark end of clone recovery process. */
  recovery_in_progress= false;
  ut_set_assert_callback(callback_function);

  std::string file_name;

  file_name.assign(CLONE_INNODB_RECOVERY_FILE);
  if (!file_exists(file_name))
    return;

  std::ofstream status_file;
  status_file.open(file_name, std::ofstream::app);
  if (!status_file.is_open())
    return;

  /* Write zero for unsuccessful recovery. */
  uint64_t end_time = 0;
  if (is_error)
  {
    status_file << end_time << std::endl;
    status_file.close();
    return;
  }

  /* Write recovery end time */
  end_time= microsecond_interval_timer();
  status_file << end_time << std::endl;
  if (!status_file.good())
  {
    status_file.close();
    return;
  }

  mtr_t mtr;
  mtr.start();
  const buf_block_t* sys_blk= trx_sysf_get(&mtr);
  byte *binlog_pos= buf_block_get_frame(sys_blk) +
                    TRX_SYS + TRX_SYS_MYSQL_LOG_INFO;
  /* Check logfile magic number. */
  if (mach_read_from_4(binlog_pos + TRX_SYS_MYSQL_LOG_MAGIC_N_FLD) !=
      TRX_SYS_MYSQL_LOG_MAGIC_N)
  {
    mtr.commit();
    status_file.close();
    return;
  }
  /* Write binary log file name. */
  status_file << binlog_pos + TRX_SYS_MYSQL_LOG_NAME << std::endl;
  if (!status_file.good())
  {
    mtr.commit();
    status_file.close();
    return;
  }
  uint64_t log_offset= mach_read_from_8(binlog_pos + TRX_SYS_MYSQL_LOG_OFFSET);

  /* Write log file offset. */
  status_file << log_offset << std::endl;

  mtr.commit();
  status_file.close();
}

/** Initialize recovery status for cloned recovery.
@param[in]      replace         we are replacing current directory. */
static void clone_init_recovery_status(bool replace)
{
  std::string file_name;
  file_name.assign(CLONE_INNODB_RECOVERY_FILE);

  std::ofstream status_file;
  status_file.open(file_name, std::ofstream::out | std::ofstream::trunc);
  if (!status_file.is_open())
    return;
  /* Write recovery begin time */
  uint64_t begin_time= microsecond_interval_timer();
  status_file << begin_time << std::endl;
  status_file.close();
  clone_update_recovery_status(false, false, replace);
}

/** Type of function which is supposed to handle a single file during
Clone operations, accepting the file's name (string).
@see clone_files_for_each_file */
typedef std::function<void(const std::string &)> Clone_file_handler;

/** Processes each file name listed in the given status file, executing a given
function for each of them.
@param[in]  status_file_name    status file name
@param[in]  process             the given function, accepting file name string
@return true iff status file was successfully opened */
static bool clone_files_for_each_file(const char *status_file_name,
                                      const Clone_file_handler &process)
{
  std::ifstream files;
  files.open(status_file_name);
  if (!files.is_open())
    return false;

  std::string data_file;
  /* Extract and process all files listed in file with name=status_file_name */
  while (std::getline(files, data_file))
    process(data_file);

  files.close();
  return true;
}

/** Process all entries and remove status file.
@param[in]      file_name       status file name
@param[in]      process         callback to process entries */
static void process_remove_file(const char *file_name,
                                const Clone_file_handler &process)
{
  if (clone_files_for_each_file(file_name, process))
  {
    std::string file_str(file_name);
    remove_file(file_str);
  }
}

void clone_files_error()
{
  /* Check if clone file directory exists. */
  if (!os_file_exists(CLONE_FILES_DIR))
    return;

  std::string err_file(CLONE_INNODB_ERROR_FILE);

  /* Create error status file if not there. */
  if (!file_exists(err_file))
    create_file(err_file);

  /* Process all old files to be moved. */
  Clone_file_handler cbk = old_file_rollback;
  process_remove_file(CLONE_INNODB_OLD_FILES, cbk);

  /* Process all files to be replaced. */
  cbk= file_rollback;
  process_remove_file(CLONE_INNODB_REPLACED_FILES, cbk);

  /* Process all new files to be deleted. */
  cbk= remove_file;
  process_remove_file(CLONE_INNODB_NEW_FILES, cbk);

  /* Process all temp ddl files to be deleted. */
  process_remove_file(CLONE_INNODB_DDL_FILES, cbk);

  /* Remove error status file. */
  remove_file(err_file);

  /* Update recovery status file for recovery error. */
  clone_update_recovery_status(true, true, true);
}

#ifdef UNIV_DEBUG
bool clone_check_recovery_crashpoint(bool is_cloned_db)
{
  if (!is_cloned_db)
    return true;

  std::string crash_file(CLONE_INNODB_RECOVERY_CRASH_POINT);

  if (file_exists(crash_file))
  {
    remove_file(crash_file);
    return false;
  }
  return true;
}
#endif

void clone_files_recovery(bool finished)
{
  /* Clone error file is present in case of error. */
  std::string file_name;
  file_name.assign(CLONE_INNODB_ERROR_FILE);

  if (file_exists(file_name))
  {
    ut_ad(!finished);
    clone_files_error();
    return;
  }

  /* if replace file is not present, remove old file. */
  if (!finished)
  {
    std::string replace_files(CLONE_INNODB_REPLACED_FILES);
    std::string old_files(CLONE_INNODB_OLD_FILES);
    if (!file_exists(replace_files) && file_exists(old_files))
    {
      remove_file(old_files);
      ut_d(ut_error);
    }
  }

  /* Open files to get all old files to be saved or removed. Must handle
  the old files before cloned files. This is because during old file
  processing we need to skip the common files based on cloned state. If
  the cloned state is reset then these files would be considered as old
  files and removed. */
  int end_state = finished ? FILE_STATE_NONE : FILE_STATE_SAVED;

  auto old_file_handler= [end_state](const std::string &fname)
  {
    old_file_roll_forward(fname, end_state);
  };

  if (clone_files_for_each_file(CLONE_INNODB_OLD_FILES, old_file_handler))
  {
    /* Remove clone file after successful recovery. */
    if (finished)
    {
      std::string old_files(CLONE_INNODB_OLD_FILES);
      remove_file(old_files);
    }
  }

  /* Open file to get all files to be replaced. */
  end_state= finished ? FILE_STATE_NORMAL : FILE_STATE_REPLACED;

  std::ifstream files;
  files.open(CLONE_INNODB_REPLACED_FILES);

  if (files.is_open())
  {
    int prev_state= FILE_STATE_NORMAL;
    /* If file is empty, it is not replace. */
    bool replace= false;

    /* Extract and process all files to be replaced */
    while (std::getline(files, file_name))
    {
      replace= true;
      prev_state= file_roll_forward(file_name, end_state);
    }
    files.close();

    if (finished)
      /* Update recovery status file at the end of clone recovery. We don't
      remove the replace file here. It would be removed only after updating
      GTID state. */
      clone_update_recovery_status(true, false, replace);
    else
    {
      /* If previous state was normal, clone recovery is already done. */
      if (!replace || prev_state != FILE_STATE_NORMAL)
        /* Clone database recovery is started. */
        clone_init_recovery_status(replace);
    }
  }
  file_name.assign(CLONE_INNODB_NEW_FILES);
  auto exists= file_exists(file_name);

  if (exists && finished)
  {
    /* Remove clone file after successful recovery. */
    std::string new_files(CLONE_INNODB_NEW_FILES);
    remove_file(new_files);
  }
}

dberr_t clone_init()
{
  /* Check if incomplete cloned data directory */
  if (os_file_exists(CLONE_INNODB_IN_PROGRESS_FILE))
    return DB_ABORT_INCOMPLETE_CLONE;

  ignore_db_dirs_append(CLONE_FILES_DIR);
  /* Initialize clone files before starting recovery. */
  clone_files_recovery(false);

  if (clone_sys == nullptr)
  {
    ut_ad(Clone_Sys::s_clone_sys_state == CLONE_SYS_INACTIVE);
    clone_sys= UT_NEW(Clone_Sys(), mem_key_clone);
  }
  Clone_Sys::s_clone_sys_state= CLONE_SYS_ACTIVE;

  return DB_SUCCESS;
}

void clone_free()
{
  if (clone_sys != nullptr)
  {
    ut_ad(Clone_Sys::s_clone_sys_state == CLONE_SYS_ACTIVE);
    UT_DELETE(clone_sys);
    clone_sys = nullptr;
  }
  Clone_Sys::s_clone_sys_state= CLONE_SYS_INACTIVE;
}

bool clone_check_provisioning() { return Clone_handler::is_provisioning(); }

bool clone_check_active()
{
  mysql_mutex_lock(clone_sys->get_mutex());
  auto is_active= clone_sys->check_active_clone(false);
  mysql_mutex_unlock(clone_sys->get_mutex());

  return (is_active || Clone_handler::is_provisioning());
}

/** TODO: Fix schema, table and tablespace. Used for two different purposes.
1. After recovery from cloned database:
A. Create empty data file for non-Innodb tables that are not cloned.
B. Create any schema directory that is not present.

2. Before cloning into current data directory:
A. Drop all user tables.
B. Drop all user schema
C. Drop all user tablespaces.

TODO: class Fixup_data
      fix_cloned_tables()
      clone_execute_query()
      clone_drop_binary_logs()
      clone_drop_user_data() */

Clone_notify::Clone_notify(Clone_notify::Type type, space_id_t space,
                           bool no_wait)
    : m_space_id(space),
      m_type(type),
      m_wait(Wait_at::NONE),
      m_blocked_state(),
      m_error()
{
  DEBUG_SYNC_C("clone_notify_ddl");

  if (fsp_is_system_temporary(space) || m_type == Type::SPACE_ALTER_INPLACE)
    /* No need to block clone. */
    return;

  std::string ntfn_mesg;
  Mysql_mutex_guard sys_mutex(clone_sys->get_mutex());

  bool clone_active= false;
  Clone_Handle *clone_donor= nullptr;

  std::tie(clone_active, clone_donor)= clone_sys->check_active_clone();

  /* This is for special case when clone_ddl_timeout is set to zero. DDL
  needs to abort any running clone in this case. */
  if (clone_active && clone_donor->abort_by_ddl())
  {
    clone_sys->mark_abort(true);
    m_wait= Wait_at::ABORT;
    return;
  }

  if (type == Type::SYSTEM_REDO_RESIZE || type == Type::SPACE_IMPORT)
  {
    if (clone_active)
    {
      get_mesg(true, ntfn_mesg);
      ib::info() << "Clone DDL Notification: " << ntfn_mesg;

      m_error = ER_CLONE_IN_PROGRESS;
      my_error(ER_CLONE_IN_PROGRESS, MYF(0));
      return;
    }
    clone_sys->mark_abort(false);
    m_wait= Wait_at::ABORT;
    return;
  }

  if (!clone_active)
  {
    /* Let any new clone block at the beginning. */
    clone_sys->mark_wait();
    m_wait = Wait_at::ENTER;
    return;
  }

  bool abort_if_failed= false;

  if (type == Type::SPACE_ALTER_ENCRYPT_GENERAL ||
      type == Type::SPACE_ALTER_ENCRYPT_GENERAL_FLAGS)
    /* For general tablespace, Encryption of data pages are always rolled
    forward as of today. Since we cannot rollback the DDL, clone is aborted
    on any failure here. */
    abort_if_failed = true;

  else if (type == Type::SPACE_DROP)
    /* Post DDL operations should not fail, the transaction is already
    committed. */
    abort_if_failed = true;

  get_mesg(true, ntfn_mesg);
  ib::info() << "Clone DDL Notification: " << ntfn_mesg;

  DEBUG_SYNC_C("clone_notify_ddl_before_state_block");

  /* Check if clone needs to block at state change. */
  if (clone_sys->begin_ddl_state(m_type, m_space_id, no_wait, true,
                                 m_blocked_state, m_error))
  {
    m_wait= Wait_at::STATE_CHANGE;
    ut_ad(!failed());
    return;
  }

  DEBUG_SYNC_C("clone_notify_ddl_after_state_block");

  DBUG_EXECUTE_IF("clone_ddl_error_abort", abort_if_failed = true;);

  /* Abort clone on failure, if requested. This is required when caller cannot
  rollback on failure. Currently enable & disable encryption needs this. In
  this case we need to force clone to abort. */
  if (failed() && abort_if_failed)
  {
    /* Clear any error raised. */
    m_error= 0;
    auto thd= current_thd;
    if (thd != nullptr)
    {
      thd->clear_error();
      thd->get_stmt_da()->reset_diagnostics_area();
    }
    clone_sys->mark_abort(true);
    m_wait= Wait_at::ABORT;
    return;
  }
  ut_ad(m_wait == Wait_at::NONE);
}

Clone_notify::~Clone_notify()
{
  Mysql_mutex_guard sys_mutex(clone_sys->get_mutex());

  switch (m_wait)
  {
    case Wait_at::ENTER:
      clone_sys->mark_free();
      break;

    case Wait_at::STATE_CHANGE:
      clone_sys->end_ddl_state(m_type, m_space_id, m_blocked_state);
      break;

    case Wait_at::ABORT:
      clone_sys->mark_active();
      break;

    case Wait_at::NONE:
      [[fallthrough]];

    default:
      return;
  }

  if (clone_sys->check_active_clone(false))
  {
    std::string ntfn_mesg;
    get_mesg(false, ntfn_mesg);
    ib::info() << "Clone DDL Notification: " << ntfn_mesg;
  }
}

void Clone_notify::get_mesg(bool begin, std::string &mesg)
{
  if (begin)
    mesg.assign("BEGIN ");
  else
    mesg.assign("END ");

  switch (m_type)
  {
    case Type::SPACE_CREATE:
      mesg.append("[SPACE_CREATE] ");
      break;
    case Type::SPACE_DROP:
      mesg.append("[SPACE_DROP] : ");
      break;
    case Type::SPACE_RENAME:
      mesg.append("[SPACE_RENAME] ");
      break;
    case Type::SPACE_ALTER_ENCRYPT:
      mesg.append("[SPACE_ALTER_ENCRYPT] ");
      break;
    case Type::SPACE_IMPORT:
      mesg.append("[SPACE_IMPORT] ");
      break;
    case Type::SPACE_ALTER_ENCRYPT_GENERAL:
      mesg.append("[SPACE_ALTER_ENCRYPT_GENERAL] ");
      break;
    case Type::SPACE_ALTER_ENCRYPT_GENERAL_FLAGS:
      mesg.append("[SPACE_ALTER_ENCRYPT_GENERAL_FLAGS] ");
      break;
    case Type::SPACE_ALTER_INPLACE:
      mesg.append("[SPACE_ALTER_INPLACE] ");
      break;
    case Type::SPACE_ALTER_INPLACE_BULK:
      mesg.append("[SPACE_ALTER_INPLACE_BULK] ");
      break;
    case Type::SYSTEM_REDO_RESIZE:
      mesg.append("[SYSTEM_REDO_RESIZE] ");
      break;
    case Type::SPACE_UNDO_TRUNCATE:
      mesg.append("[SPACE_UNDO_TRUNCATE] ");
      break;
    default:
      mesg.append("[UNKNOWN] ");
      break;
  }

  if (m_space_id == UINT32_MAX)
    return;

  mesg.append("Space ID: ");
  mesg.append(std::to_string(m_space_id));

  auto fil_space = fil_space_get(m_space_id);
  if (fil_space == nullptr)
    return;

  auto node= UT_LIST_GET_FIRST(fil_space->chain);
  mesg.append(" File: ");
  mesg.append(node->name);
}

static int clone_init_tablespaces(THD *thd)
{
  if (clone_sys->is_space_initialized())
    return 0;

  /* TODO: Invoke BLOCK DDL MDL Lock service call. */
  /* We need to acquire X backup lock here to prevent DDLs. Clone by default
  skips DDL lock. The API can handle recursive calls and it is not an issue
  if clone has already acquired backup lock. */
  // auto timeout= static_cast<ulong>(get_ddl_timeout(thd));

  // if (acquire_exclusive_backup_lock(thd, timeout, false))
  // {
    /* Timeout on backup lock. */
  //   my_error(ER_LOCK_WAIT_TIMEOUT, MYF(0));
  //   return ER_LOCK_WAIT_TIMEOUT;
  // }
  ib::info() << "Clone: Started loading tablespaces";

  dict_load_spaces_no_ddl();

  // release_backup_lock(thd);
  clone_sys->set_space_initialized();

  ib::info() << "Clone: Finished loading tablespaces";
  return 0;
}

Clone_Sys::Wait_stage::Wait_stage(const char *new_info)
{
  m_saved_info= nullptr;
  THD *thd= current_thd;

  if (thd != nullptr)
  {
    m_saved_info= thd->get_proc_info();
    thd->proc_info= new_info;
  }
}

Clone_Sys::Wait_stage::~Wait_stage()
{
  THD *thd= current_thd;

  if (thd != nullptr && m_saved_info != nullptr)
    thd->proc_info= m_saved_info;
}
