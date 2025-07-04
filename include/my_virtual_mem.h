/* Copyright (c) 2025, MariaDB

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
/*
  Functionality for handling virtual memory
  (reserve, commit, decommit, release)
*/
#include <stddef.h> /*size_t*/

#ifdef __cplusplus
extern "C" {
#endif

# ifdef _WIN32
char *my_virtual_mem_reserve(size_t *size);
# endif
char *my_virtual_mem_commit(char *ptr, size_t size);
void my_virtual_mem_decommit(char *ptr, size_t size);
void my_virtual_mem_release(char *ptr, size_t size);

#ifdef __cplusplus
}
#endif

