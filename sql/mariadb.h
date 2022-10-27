/* Copyright (c) 2010, 2017, MariaDB Corporation.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA */

/*
  Include file that should always be included first in all file in the sql
  directory. Used to ensure that some files, like my_global.h and my_config.h
  are always included first.
  It can also be used to speed up compilation by using precompiled headers.

  This file should include a minum set of header files used by all files
  and header files that are very seldom changed.
  It can also include some defines that all files should be aware of.
*/

#ifndef MARIADB_INCLUDED
#define MARIADB_INCLUDED
#include <my_global.h>
#endif /* MARIADB_INCLUDED */
