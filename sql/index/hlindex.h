/*
   Copyright (c) 2026, MariaDB plc

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1335  USA
*/

#include "handler.h"

/*
  singleton with index options and tabledef
  `- hlindexton
  in TABLE_SHARE - TABLE_SHARE and shared context
  `- hlindex_share
  in TABLE - TABLE or read context
  `- hlindex

*/

class hlindex : public Sql_alloc
{
public:
  hlindex(TABLE *t) : table(t) { }

  virtual int insert_row(TABLE *table, KEY *keyinfo) = 0;
  virtual int read_key(TABLE *table, KEY *keyinfo, Item *dist, ulonglong limit) = 0;
  virtual int read_next(TABLE *table) = 0;
  virtual int read_end(TABLE *table) = 0;
  virtual int delete_row(TABLE *table, const uchar *rec, KEY *keyinfo) = 0;
  virtual int delete_all(TABLE *table, KEY *keyinfo, bool truncate) = 0;
  virtual bool reading() = 0;
  virtual ~hlindex();

  TABLE *table;
};

class hlindex_share : public Sql_alloc
{
public:
  virtual hlindex *create(TABLE *table, MEM_ROOT *mem_root) = 0;
  virtual ~hlindex_share() = default;

  TABLE_SHARE *s;
};

struct hlindexton : public transaction_participant
{
  ha_create_table_option *options;
  const LEX_CSTRING (*table_def)(THD *thd, uint ref_length);
  //hlindex *(*create)(TABLE_SHARE *table, MEM_ROOT *mem_root);
  hlindex_share *(*create)(TABLE_SHARE *share, MEM_ROOT *mem_root);
};
