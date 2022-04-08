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

#define SPIDER_NET_HAS_THD
#define HANDLER_HAS_TOP_TABLE_FIELDS
#define PARTITION_HAS_GET_PART_SPEC
#define HA_EXTRA_HAS_STARTING_ORDERED_INDEX_SCAN
#define HANDLER_HAS_CAN_USE_FOR_AUTO_INC_INIT
#define SPIDER_UPDATE_ROW_HAS_CONST_NEW_DATA
#define SPIDER_MDEV_16246
#define SPIDER_LIKE_FUNC_HAS_GET_NEGATED
#define SPIDER_I_S_USE_SHOW_FOR_COLUMN
#endif /* SPD_ENVIRON_INCLUDED */
