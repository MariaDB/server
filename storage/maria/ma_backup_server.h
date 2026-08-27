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

#include "sql_backup_interface.h"

#ifdef __cplusplus
extern "C"
#endif
/**
   Start of a BACKUP SERVER phase,
   when no aria_backup_step() or aria_backup_end() is pending.
   @param thd     current session
   @param target  backup target
   @param phase   BACKUP_PHASE_START, ... (not BACKUP_PHASE_ABORT)
   @param sink    worker context
   @return backup context object to be attached to backup_target
   @retval NULL   if no context needs to be created
   @retval -1     on failure
*/
void *aria_backup_start(THD *thd, const struct backup_target *target,
                        enum backup_phase phase,
                        const struct backup_sink *sink);

#ifdef __cplusplus
extern "C"
#endif
/**
   Process a file that was collected in aria_backup_start().
   @param thd   current session
   @param target  backup target
   @param phase   last phase on which backup_start() was successfully invoked
   @param sink    worker context
   @return number directories or files left; negative on error
   @retval 0 on successful completion
*/
int aria_backup_step(THD *thd, const struct backup_target *target,
                     enum backup_phase phase, const struct backup_sink *sink);

#ifdef __cplusplus
extern "C"
#endif
/**
   Finish a phase, once all calls for the current phase are completed.
   @param thd   current session
   @param target  backup target
   @param phase   last phase on which backup_start() was successfully invoked,
   or BACKUP_PHASE_ABORT or BACKUP_PHASE_FINISH
   @param sink    worker context
   @return error code
   @retval 0 on success
*/
int aria_backup_end(THD *thd, const struct backup_target *target,
                    enum backup_phase phase, const struct backup_sink *sink);
