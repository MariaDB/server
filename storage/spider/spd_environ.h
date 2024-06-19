/* Copyright (C) 2008-2020 Kentoku Shiba
   Copyright (C) 2017-2020 MariaDB corp

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

/*
  Define functionality offered by MySQL or MariaDB
*/

#ifndef SPD_ENVIRON_INCLUDED

#define SPIDER_SUPPORT_CREATE_OR_REPLACE_TABLE

#define HANDLER_HAS_DIRECT_AGGREGATE

#define SPIDER_USE_CONST_ITEM_FOR_STRING_INT_REAL_DECIMAL_DATE_ITEM
#define SPIDER_SQL_CACHE_IS_IN_LEX
#define SPIDER_LIKE_FUNC_HAS_GET_NEGATED
#define HA_HAS_CHECKSUM_EXTENDED

#define SPIDER_I_S_USE_SHOW_FOR_COLUMN
#endif /* SPD_ENVIRON_INCLUDED */
