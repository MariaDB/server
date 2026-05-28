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

#include <my_global.h>
//#include "m_string.h"
//#include "structs.h"
#include "table.h"

//const LEX_CSTRING fts_hlindex_table_def(THD *thd, uint ref_length);
//int fts_insert(TABLE *table, KEY *keyinfo);
//int fts_read_first(TABLE *table, KEY *keyinfo, Item *dist, ulonglong limit);
//int fts_read_next(TABLE *table);
//int fts_read_end(TABLE *table);
//int fts_invalidate(TABLE *table, const uchar *rec, KEY *keyinfo);
//int fts_delete_all(TABLE *table, KEY *keyinfo, bool truncate);
//void fts_free(TABLE_SHARE *share);

extern ha_create_table_option fts_index_options[];
extern st_plugin_int *fts_plugin;
