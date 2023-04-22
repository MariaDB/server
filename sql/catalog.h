#ifndef CATALOG_INCLUDED
#define CATALOG_INCLUDED

/* Copyright (c) 2023, MariaDB Foundation

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1335
   USA
*/

class SQL_CATALOG
{
public:
  SQL_CATALOG(const LEX_CSTRING *name, const LEX_CSTRING *path);
  const LEX_CSTRING name;
  const LEX_CSTRING path;             // Directory path, including '/'
  ulong event_scheduler;              // Default for event scheduler
};

extern SQL_CATALOG *get_catalog(LEX_CSTRING *name);
extern const LEX_CSTRING default_catalog_name;
extern SQL_CATALOG internal_default_catalog;
inline SQL_CATALOG *default_catalog()
{
  return &internal_default_catalog;
}

extern my_bool using_catalogs;

#endif /* CATALOG_INCLUDED */
