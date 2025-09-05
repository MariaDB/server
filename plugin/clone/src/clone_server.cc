/* Copyright (c) 2017, 2024, Oracle and/or its affiliates.

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
@file clone/src/clone_server.cc
Clone Plugin: Server implementation

*/

#include "clone_server.h"
#include "clone_status.h"
#include "log.h"

#include "my_byteorder.h"

/* Namespace for all clone data types */
namespace myclone {

Server::Server(THD *thd)
    : m_server_thd(thd),
      m_is_master(false),
      m_acquired_backup_lock(false),
      m_protocol_version(CLONE_PROTOCOL_VERSION) {
}

Server::~Server() {
}

int Server::get_stage_and_lock(Sub_Command sub_cmd, Ha_clone_stage &stage,
                               bool lock)
{
  int err= 0;
  THD *thd= get_thd();
  const char *err_msg= nullptr;
  switch (sub_cmd)
  {
    case SUBCOM_EXEC_CONCURRENT:
      if (lock)
      {
        log_error(thd, false, 0, "Acquiring locks for BACKUP STAGE "
                                 "START");
        err= clone_set_backup_stage(thd, START);
        if (err)
        {
          err_msg= "Failed to acquire locks for BACKUP STAGE START";
          goto err_exit;
        }
        log_error(thd, false, 0, "Acquired locks for BACKUP STAGE "
                                 "START");
        DEBUG_SYNC_C("backup_stage_start");
        m_acquired_backup_lock= true;
      }
      stage= HA_CLONE_STAGE_CONCURRENT;
      break;
    case SUBCOM_EXEC_BLOCK_NT_DML:
      if (lock)
      {
        assert(m_acquired_backup_lock);
        log_error(thd, false, 0, "Acquiring locks for BACKUP STAGE "
                                 "FLUSH");
        err= clone_set_backup_stage(thd, FLUSH);
	if (err)
	{
          err_msg= "Failed to acquire locks for BACKUP STAGE FLUSH";
          goto err_exit;
	}
	log_error(thd, false, 0, "Acquired locks for BACKUP STAGE "
                                 "FLUSH");
      }
      stage= HA_CLONE_STAGE_NT_DML_BLOCKED;
      break;
    case SUBCOM_EXEC_BLOCK_DDL:
      if (lock)
      {
        assert(m_acquired_backup_lock);
        log_error(thd, false, 0, "Acquiring locks for BACKUP STAGE "
                                 "BLOCK_DDL");
        err= clone_set_backup_stage(thd, BLOCK_DDL);
	if (err)
	{
          err_msg= "Failed to acquire locks for BACKUP STAGE BLOCK_DDL";
          goto err_exit;
	}
	log_error(thd, false, 0, "Acquired locks for BACKUP STAGE "
                                 "BLOCK_DDL");
      }
      stage= HA_CLONE_STAGE_DDL_BLOCKED;
      break;
    case SUBCOM_EXEC_SNAPSHOT:
      assert(lock);
      assert(m_acquired_backup_lock);
      log_error(thd, false, 0, "Acquiring locks for BACKUP STAGE "
                               "BLOCK_COMMIT");
      err= clone_set_backup_stage(thd, BLOCK_COMMIT);
      if (err)
      {
        err_msg= "Failed to acquire locks for BACKUP STAGE BLOCK_COMMIT";
        goto err_exit;
      }
      log_error(thd, false, 0, "Acquired locks for BACKUP STAGE "
                               "BLOCK_COMMIT");
      stage= HA_CLONE_STAGE_SNAPSHOT;
      break;
    case SUBCOM_EXEC_END:
      /* The function could be invoked with SUBCOM_EXEC_END for error cleanup.
      We need to check and unlock only if needed. */
      if (lock && m_acquired_backup_lock)
      {
        log_error(thd, false, 0, "Executing BACKUP STAGE END");
        err= clone_set_backup_stage(thd, END);
        if (err)
        {
          err_msg= "Failed to release BACKUP LOCKS";
          goto err_exit;
        }
        log_error(thd, false, 0, "Released BACKUP LOCKS");
      }
      stage= HA_CLONE_STAGE_END;
      break;
    case SUBCOM_MAX:
    case SUBCOM_NONE:
      err= ER_CLONE_PROTOCOL;
      my_error(err, MYF(0), "Wrong Clone RPC: Invalid Execution Request");
      log_error(get_thd(), false, err, "COM_EXECUTE");
  }
err_exit:
  if (err_msg)
  {
    err= ER_INTERNAL_ERROR;
    my_error(err, MYF(0), err_msg);
  }
  return err;
}
}  // namespace myclone
