/*
  Copyright (c) 2026, MariaDB Foundation.
  Copyright (c) 2026, Roman Nozdrin <drrtuy@gmail.com>
  Copyright (c) 2026, Leonid Fedorov.

  This program is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation; version 2 of the License.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program; if not, write to the Free Software
  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1335 USA
*/

#pragma once

#include <string>

/**
  Utility class to parse a full path like "./db/table" into db_name and
  table_name components. Handles escape characters in names.
*/
class DatabaseTableNames
{
public:
  DatabaseTableNames(const char *name);
  std::string db_name;
  std::string table_name;
};

/**
  Utility class to extract the database name from a path like "./db/".
*/
class Databasename
{
public:
  Databasename(const char *path_name);
  std::string name;
};
