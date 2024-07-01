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


const LEX_CSTRING ivflfat_hlindex_table_def(THD *thd, uint ref_length);
// extern const LEX_CSTRING ivfflat_hlindex_table;
int ivfflat_insert(TABLE *table, KEY *keyinfo);
int ivfflat_first(TABLE *table, KEY *keyinfo, Item *dist, ulonglong limit);
int ivfflat_next(TABLE *table);

