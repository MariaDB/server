/* Copyright (c) 2022 MariaDB

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
#ifdef _WIN32
#define CREDMGR_SUPPORTED
char *credmgr_make_target(char *out, size_t sz, const char *host,
                          const char *user, uint port,
                          const char *unix_socket);
char *credmgr_get_password(const char *target_name);
void credmgr_save_password(const char *target_name, const char *password);
void credmgr_remove_password(const char *target_name);
#endif
