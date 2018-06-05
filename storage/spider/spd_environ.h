/* Copyright (C) 2008-2017 Kentoku Shiba & 2017 MariaDB corp

  This program is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation; version 2 of the License.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program; if not, write to the Free Software
  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02111-1301 USA */

/*
  Define functionality offered by MySQL or MariaDB
*/

#ifndef SPD_ENVIRON_INCLUDED

#if (defined(MARIADB_BASE_VERSION) && MYSQL_VERSION_ID >= 100000)
#define SPIDER_HANDLER_START_BULK_INSERT_HAS_FLAGS
#endif

#if defined(MARIADB_BASE_VERSION) && MYSQL_VERSION_ID >=	100100
#define SPIDER_SUPPORT_CREATE_OR_REPLACE_TABLE
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
#endif /* SPD_ENVIRON_INCLUDED */
