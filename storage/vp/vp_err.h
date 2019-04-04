/* Copyright (C) 2009-2014 Kentoku Shiba

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

#define ER_VP_INVALID_TABLE_INFO_NUM 14501
#define ER_VP_INVALID_TABLE_INFO_STR "The table info '%-.64s' is invalid"
#define ER_VP_INVALID_TABLE_INFO_TOO_LONG_NUM 14502
#define ER_VP_INVALID_TABLE_INFO_TOO_LONG_STR "The table info '%-.64s' for %s is too long"
#define ER_VP_INVALID_UDF_PARAM_NUM 14503
#define ER_VP_INVALID_UDF_PARAM_STR "The UDF parameter '%-.64s' is invalid"
#define ER_VP_BLANK_UDF_ARGUMENT_NUM 14504
#define ER_VP_BLANK_UDF_ARGUMENT_STR "The UDF no.%d argument can't be blank"
#define ER_VP_TBL_NUM_OUT_OF_RANGE_NUM 14505
#define ER_VP_TBL_NUM_OUT_OF_RANGE_STR "Table index %d is out of range"
#define ER_VP_CANT_CORRESPOND_TABLE_NUM 14511
#define ER_VP_CANT_CORRESPOND_TABLE_STR "Can't correspond table '%s'"
#define ER_VP_CANT_CORRESPOND_COLUMN_NUM 14512
#define ER_VP_CANT_CORRESPOND_COLUMN_STR "Can't correspond column '%s'"
#define ER_VP_CANT_CORRESPOND_KEY_NUM 14513
#define ER_VP_CANT_CORRESPOND_KEY_STR "Can't correspond key '%d'"
#define ER_VP_CANT_CORRESPOND_PK_NUM 14514
#define ER_VP_CANT_CORRESPOND_PK_STR "Can't correspond PK '%s'"
#define ER_VP_CANT_CORRESPOND_AUTO_INC_NUM 14515
#define ER_VP_CANT_CORRESPOND_AUTO_INC_STR "Can't correspond auto_increment column '%s'"
#define ER_VP_DIFFERENT_COLUMN_TYPE_NUM 14516
#define ER_VP_DIFFERENT_COLUMN_TYPE_STR "Different column type '%s'.'%s'"
#define ER_VP_IGNORED_CORRESPOND_KEY_NUM 14517
#define ER_VP_IGNORED_CORRESPOND_KEY_STR "Key no.%d is on ignored tables"
#define ER_VP_IGNORED_CORRESPOND_COLUMN_NUM 14518
#define ER_VP_IGNORED_CORRESPOND_COLUMN_STR "Some columns are on ignored tables"
#define ER_VP_UDF_CANT_USE_IF_OPEN_TABLE_NUM 14521
#define ER_VP_UDF_CANT_USE_IF_OPEN_TABLE_STR "This UDF can't execute if other tables are opened"
#define ER_VP_UDF_CANT_OPEN_TABLE_NUM 14521
#define ER_VP_UDF_CANT_OPEN_TABLE_STR "Can't open tables"
#define ER_VP_UDF_IS_NOT_VP_TABLE_NUM 14522
#define ER_VP_UDF_IS_NOT_VP_TABLE_STR "Target table(argument 1) is not Vertical Partitioning table"
#define ER_VP_UDF_CANT_FIND_TABLE_NUM 14523
#define ER_VP_UDF_CANT_FIND_TABLE_STR "Can't find child table '%s.%s' in VP table"
#define ER_VP_UDF_FIND_SAME_TABLE_NUM 14524
#define ER_VP_UDF_FIND_SAME_TABLE_STR "Can't use same table at source and destination"
#define ER_VP_UDF_FIND_CHANGE_TABLE_NUM 14525
#define ER_VP_UDF_FIND_CHANGE_TABLE_STR "Change table definition during this executing"
#define ER_VP_UDF_MUST_SET_ZRU_NUM 14526
#define ER_VP_UDF_MUST_SET_ZRU_STR "You must set zero_record_update_mode = 1 for table '%s.%s'"

#define ER_VP_COND_SKIP_NUM 14551

#define ER_VP_UNKNOWN_STR "unknown"
#define ER_VP_UNKNOWN_LEN (sizeof(ER_VP_UNKNOWN_STR) - 1)
