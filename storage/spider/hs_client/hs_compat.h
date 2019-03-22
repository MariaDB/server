/* Copyright (C) 2013-2018 Kentoku Shiba

  This program is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation; version 2 of the License.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program; if not, write to the Free Software
  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA */

#ifndef HS_COMPAT_H
#define HS_COMPAT_H

#if defined(MARIADB_BASE_VERSION) && MYSQL_VERSION_ID >= 100213
#define SPD_INIT_DYNAMIC_ARRAY2(A, B, C, D, E, F) \
  my_init_dynamic_array2(A, B, C, D, E, F)
#define SPD_INIT_ALLOC_ROOT(A, B, C, D) \
  init_alloc_root(A, "spider", B, C, D)
#elif defined(MARIADB_BASE_VERSION) && MYSQL_VERSION_ID >= 100000
#define SPD_INIT_DYNAMIC_ARRAY2(A, B, C, D, E, F) \
  my_init_dynamic_array2(A, B, C, D, E, F)
#define SPD_INIT_ALLOC_ROOT(A, B, C, D) \
  init_alloc_root(A, B, C, D)
#else
#define SPD_INIT_DYNAMIC_ARRAY2(A, B, C, D, E, F) \
  my_init_dynamic_array2(A, B, C, D, E)
#define SPD_INIT_ALLOC_ROOT(A, B, C, D) \
  init_alloc_root(A, B, C)
#endif

#endif
