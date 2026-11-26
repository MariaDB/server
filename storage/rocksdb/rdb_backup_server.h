/* Copyright (c) 2026, MariaDB plc

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

#pragma once

/* BACKUP SERVER support for the RocksDB (MyRocks) engine. */

namespace myrocks {

/**
   Start of a BACKUP SERVER phase.
   @param thd     current session
   @param target  backup target
   @param stage   BACKUP_LOCK_COMMIT to make snapshot;
   BACKUP_FINISHED to prepare for copying
   @param sink    worker context
   @return backup context object to be attached to sink, or nullptr
   @retval -1     on failure
*/
void *backup_start(THD *thd, const backup_target *target,
                   backup_stages stage, const backup_sink *sink)
  noexcept;

/**
   Back up a file of the checkpoint that was frozen in rocksdb_backup_start().
   @param thd     current session
   @param target  backup target
   @param stage   last stage on which backup_start() was successfully invoked
   @param sink    worker context
   @retval 1 on success if some work remains
   @retval 0 on completion
   @retval -1 on error
*/
int backup_step(THD *thd, const backup_target *target,
                backup_stages stage, const backup_sink *sink) noexcept;

/**
   Finish a phase, once all calls for the current phase are completed.
   @param thd     current session
   @param target  backup target
   @param stage   last stage on which backup_start() was successfully invoked,
   or BACKUP_ABORTED or BACKUP_FINISHED
   @param sink    worker context
   @return error code
   @retval 0 on success
*/
int backup_end(THD *thd, const backup_target *target,
               backup_stages phase, const backup_sink *sink) noexcept;

}  // namespace myrocks
