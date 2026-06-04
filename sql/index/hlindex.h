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

class hlindex { };

struct hlindexton : public transaction_participant
{
  ha_create_table_option *options;
  const LEX_CSTRING (*table_def)(THD *thd, uint ref_length);
  hlindex *(*create)(handlerton *hton, TABLE_SHARE *table, MEM_ROOT *mem_root);
};

#if 0
class hlindex
{
  int (*insert)(TABLE *table, KEY *keyinfo);
  int (*read_first)(TABLE *table, KEY *keyinfo, Item *dist, ulonglong limit);
  int (*invalidate)(TABLE *table, const uchar *rec, KEY *keyinfo);
  int (*delete_all)(TABLE *table, KEY *keyinfo, bool truncate);
  void (*free)(TABLE_SHARE *share);
  Item_func_vec_distance::distance_kind (*uses_vec_distance)(const TABLE *table, KEY *keyinfo);
};

class hlindex
{
  virtual int read_next(TABLE *table);
  virtual int read_end(TABLE *table);
};
#endif
