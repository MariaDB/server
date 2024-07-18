/*
   Copyright (c) 2024, MariaDB plc

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

#include <my_global.h>
#include "item.h"
#include "m_string.h"
#include "structs.h"
#include "table.h"

const LEX_CSTRING mhnsw_hlindex_table_def(THD *thd, uint ref_length);
int mhnsw_insert(TABLE *table, KEY *keyinfo);
int mhnsw_read_first(TABLE *table, KEY *keyinfo, Item *dist, ulonglong limit);
int mhnsw_read_next(TABLE *table);
int mhnsw_invalidate(TABLE *table, const uchar *rec, KEY *keyinfo);
int mhnsw_delete_all(TABLE *table, KEY *keyinfo);
void mhnsw_free(TABLE_SHARE *share);

extern ulonglong mhnsw_cache_size;
