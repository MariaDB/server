/* Copyright (C) 2008-2018 Kentoku Shiba & 2017 MariaDB corp

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

#if (defined(MARIADB_BASE_VERSION) && MYSQL_VERSION_ID >= 100000)
#define SPIDER_HANDLER_START_BULK_INSERT_HAS_FLAGS
#endif

#if defined(MARIADB_BASE_VERSION) && MYSQL_VERSION_ID >=	100100
#define SPIDER_SUPPORT_CREATE_OR_REPLACE_TABLE
#define SPIDER_NET_HAS_THD
#endif

#if defined(MARIADB_BASE_VERSION) && MYSQL_VERSION_ID >=	100211
#define HANDLER_HAS_TOP_TABLE_FIELDS
#define HANDLER_HAS_DIRECT_UPDATE_ROWS
#define HANDLER_HAS_DIRECT_AGGREGATE
#define PARTITION_HAS_GET_CHILD_HANDLERS
#define PARTITION_HAS_GET_PART_SPEC
#define HA_EXTRA_HAS_STARTING_ORDERED_INDEX_SCAN
#define HANDLER_HAS_NEED_INFO_FOR_AUTO_INC
#define HANDLER_HAS_CAN_USE_FOR_AUTO_INC_INIT
#endif

#if defined(MARIADB_BASE_VERSION) && MYSQL_VERSION_ID >=	100300
#define SPIDER_UPDATE_ROW_HAS_CONST_NEW_DATA
#endif

#if defined(MARIADB_BASE_VERSION) && MYSQL_VERSION_ID >=	100309
#define SPIDER_MDEV_16246
#endif

#if defined(MARIADB_BASE_VERSION) && MYSQL_VERSION_ID >=	100400
#define SPIDER_USE_CONST_ITEM_FOR_STRING_INT_REAL_DECIMAL_DATE_ITEM
#define SPIDER_SQL_CACHE_IS_IN_LEX
#define SPIDER_LIKE_FUNC_HAS_GET_NEGATED
#define HA_HAS_CHECKSUM_EXTENDED
#endif
#endif /* SPD_ENVIRON_INCLUDED */
