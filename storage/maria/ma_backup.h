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

/* BACKUP SERVER support for Aria engine. */

/**
   Start of BACKUP SERVER: collect all files to be backed up
   @param thd     current session
   @param target  target directory
   @return error code
   @retval 0 on success
*/
int aria_backup_start(THD *thd, IF_WIN(const char*,int) target) noexcept;

/**
   Process a file that was collected in backup_start().
   @param thd   current session
   @return number of files remaining, or negative on error
   @retval 0 on completion
*/
int aria_backup_step(THD *thd) noexcept;

/**
   Finish copying and determine the logical time of the backup snapshot.
   @param thd   current session
   @param abort whether BACKUP SERVER was aborted
   @return error code
   @retval 0 on success
*/
int aria_backup_end(THD *thd, bool abort) noexcept;
