/* Copyright (C) 2019 Kentoku Shiba
   Copyright (C) 2019 MariaDB corp

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

#ifndef VP_ENVIRON_INCLUDED
#define VP_ENVIRON_INCLUDED

#if defined(MARIADB_BASE_VERSION) && MYSQL_VERSION_ID >= 100000
#define VP_HANDLER_START_BULK_INSERT_HAS_FLAGS
#endif

#if defined(MARIADB_BASE_VERSION) && MYSQL_VERSION_ID >= 100203
#define HANDLER_HAS_TOP_TABLE_FIELDS
#define PARTITION_HAS_EXTRA_ATTACH_CHILDREN
#define PARTITION_HAS_GET_CHILD_HANDLERS
#define HA_EXTRA_HAS_STARTING_ORDERED_INDEX_SCAN
#endif

#if defined(MARIADB_BASE_VERSION) && MYSQL_VERSION_ID >= 100204
#define VP_FIELD_BLOB_GET_PTR_RETURNS_UCHAR_PTR
#define HANDLER_HAS_DIRECT_UPDATE_ROWS
#define HANDLER_HAS_NEED_INFO_FOR_AUTO_INC
#define HANDLER_HAS_PRUNE_PARTITIONS_FOR_CHILD
#define HANDLER_HAS_GET_NEXT_GLOBAL_FOR_CHILD
#define HANDLER_HAS_CHECK_AND_SET_BITMAP_FOR_UPDATE
#else
#ifdef HANDLER_HAS_DIRECT_UPDATE_ROWS
#define HANDLER_HAS_DIRECT_UPDATE_ROWS_WITH_HS
#endif
#endif

#if defined(MARIADB_BASE_VERSION) && MYSQL_VERSION_ID >= 100211
#define HANDLER_HAS_DIRECT_AGGREGATE
#endif

#if defined(MARIADB_BASE_VERSION) && MYSQL_VERSION_ID >= 100300
#define VP_UPDATE_ROW_HAS_CONST_NEW_DATA
#define VP_PARTITION_HAS_CONNECTION_STRING
#define VP_REGISTER_QUERY_CACHE_TABLE_HAS_CONST_TABLE_KEY
#define VP_END_BULK_UPDATE_RETURNS_INT
#endif

#if defined(MARIADB_BASE_VERSION) && MYSQL_VERSION_ID >=	100309
#define VP_MDEV_16246
#define VP_alter_table_operations alter_table_operations
#else
#define VP_alter_table_operations uint
#endif

#endif /* VP_ENVIRON_INCLUDED */
