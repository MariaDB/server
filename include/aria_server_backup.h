/* Copyright (C) 2018,2020 MariaDB Corporation Ab

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software Foundation,
   Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02111-1301 USA */

/* Interfaces for doing backups of Aria tables */

#ifndef ARIA_SERVER_BACKUP_INCLUDED
#define ARIA_SERVER_BACKUP_INCLUDED

#include <aria_backup.h>

class THD;

/* ARIA_BACKUP_CONTEXT is defined in aria_backup.h */

int aria_open_files_for_backup(THD *thd,
                               const char *path, my_bool trans_type,
                               ARIA_BACKUP_CONTEXT *context);
void aria_close_files_for_backup(ARIA_BACKUP_CONTEXT *context);

#endif /* ARIA_SERVER_BACKUP_INCLUDED */
