#ifndef BACKUP_INCLUDED
#define BACKUP_INCLUDED
/* Copyright (c) 2018, MariaDB Corporation

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA */

enum backup_stages
{
  BACKUP_START, BACKUP_FLUSH, BACKUP_WAIT_FOR_FLUSH, BACKUP_LOCK_COMMIT,
  BACKUP_END, BACKUP_FINISHED
};

extern TYPELIB backup_stage_names;

void backup_init();
bool run_backup_stage(THD *thd, backup_stages stage);
bool backup_end(THD *thd);
void backup_set_alter_copy_lock(THD *thd, TABLE *altered_table);
bool backup_reset_alter_copy_lock(THD *thd);

bool backup_lock(THD *thd, TABLE_LIST *table);
void backup_unlock(THD *thd);
#endif /* BACKUP_INCLUDED */
