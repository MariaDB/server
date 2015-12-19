/* Copyright (C) 2008-2014 Kentoku Shiba

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

#define ER_SPIDER_INVALID_CONNECT_INFO_NUM 12501
#define ER_SPIDER_INVALID_CONNECT_INFO_STR "The connect info '%-.64s' is invalid"
#define ER_SPIDER_INVALID_CONNECT_INFO_TOO_LONG_NUM 12502
#define ER_SPIDER_INVALID_CONNECT_INFO_TOO_LONG_STR "The connect info '%-.64s' for %s is too long"
#define ER_SPIDER_INVALID_UDF_PARAM_NUM 12503
#define ER_SPIDER_INVALID_UDF_PARAM_STR "The UDF parameter '%-.64s' is invalid"
#define ER_SPIDER_DIFFERENT_LINK_COUNT_NUM 12504
#define ER_SPIDER_DIFFERENT_LINK_COUNT_STR "Different multiple table link parameter's count"
#define ER_SPIDER_UDF_PING_TABLE_PARAM_TOO_LONG_NUM 12505
#define ER_SPIDER_UDF_PING_TABLE_PARAM_TOO_LONG_STR "Server name or table name are too long"
#define ER_SPIDER_UDF_PING_TABLE_PARAM_REQIRED_NUM 12506
#define ER_SPIDER_UDF_PING_TABLE_PARAM_REQIRED_STR "Server name or table name are required"
#define ER_SPIDER_UDF_CANT_USE_IF_OPEN_TABLE_NUM 12507
#define ER_SPIDER_UDF_CANT_USE_IF_OPEN_TABLE_STR "This UDF can't execute if other tables are opened"
#define ER_SPIDER_UDF_CANT_USE_IF_OPEN_TABLE_STR_WITH_NUM "This UDF can't execute if other tables are opened '%s'=%lld"
#define ER_SPIDER_UDF_CANT_USE_IF_OPEN_TABLE_STR_WITH_PTR "This UDF can't execute if other tables are opened '%s'=%p"
#define ER_SPIDER_UDF_PING_TABLE_NO_SERVER_ID_NUM 12508
#define ER_SPIDER_UDF_PING_TABLE_NO_SERVER_ID_STR "Current server_id is not exist"
#define ER_SPIDER_UDF_PING_TABLE_DIFFERENT_MON_NUM 12509
#define ER_SPIDER_UDF_PING_TABLE_DIFFERENT_MON_STR "Monitor count is different"
#define ER_SPIDER_LINK_MON_OK_NUM 12510
#define ER_SPIDER_LINK_MON_OK_STR "Table '%s.%s' get a problem, but mon is OK"
#define ER_SPIDER_LINK_MON_NG_NUM 12511
#define ER_SPIDER_LINK_MON_NG_STR "Table '%s.%s' get a problem"
#define ER_SPIDER_LINK_MON_DRAW_FEW_MON_NUM 12512
#define ER_SPIDER_LINK_MON_DRAW_FEW_MON_STR "Can not judge by enough monitor for table '%s.%s'"
#define ER_SPIDER_LINK_MON_DRAW_NUM 12513
#define ER_SPIDER_LINK_MON_DRAW_STR "Can not judge status for table '%s.%s'"
#define ER_SPIDER_ALL_LINKS_FAILED_NUM 12514
#define ER_SPIDER_ALL_LINKS_FAILED_STR "All links of '%s.%s' are failed"
#define ER_SPIDER_TMP_TABLE_MON_NUM 12515
#define ER_SPIDER_TMP_TABLE_MON_STR "Can't use monitor by temporary table"
#define ER_SPIDER_MON_AT_ALTER_TABLE_NUM 12516
#define ER_SPIDER_MON_AT_ALTER_TABLE_STR "Got an error in alter or drop table"
#define ER_SPIDER_BLANK_UDF_ARGUMENT_NUM 12517
#define ER_SPIDER_BLANK_UDF_ARGUMENT_STR "The UDF no.%d argument can't be blank"
#define ER_SPIDER_READ_ONLY_NUM 12518
#define ER_SPIDER_READ_ONLY_STR "Table '%s.%s' is read only"
#define ER_SPIDER_LINK_IS_FAILOVER_NUM 12519
#define ER_SPIDER_LINK_IS_FAILOVER_STR "A link is fail-overed"
#define ER_SPIDER_AUTOINC_VAL_IS_DIFFERENT_NUM 12520
#define ER_SPIDER_AUTOINC_VAL_IS_DIFFERENT_STR "Binlog's auto-inc value is probably different from linked table's auto-inc value"
#define ER_SPIDER_SQL_WRAPPER_IS_INVALID_NUM 12521
#define ER_SPIDER_SQL_WRAPPER_IS_INVALID_STR "Can't use wrapper '%s' for SQL connection"
#define ER_SPIDER_NOSQL_WRAPPER_IS_INVALID_NUM 12522
#define ER_SPIDER_NOSQL_WRAPPER_IS_INVALID_STR "Can't use wrapper '%s' for NOSQL connection"
#define ER_SPIDER_REQUEST_KEY_NUM 12523
#define ER_SPIDER_REQUEST_KEY_STR "Request key not found"
#define ER_SPIDER_CANT_OPEN_SYS_TABLE_NUM 12524
#define ER_SPIDER_CANT_OPEN_SYS_TABLE_STR "Can't open system table %s.%s"
#define ER_SPIDER_LINK_MON_JUST_NG_NUM 12525
#define ER_SPIDER_LINK_MON_JUST_NG_STR "Table '%s.%s' just got a problem"

#define ER_SPIDER_CANT_USE_BOTH_INNER_XA_AND_SNAPSHOT_NUM 12601
#define ER_SPIDER_CANT_USE_BOTH_INNER_XA_AND_SNAPSHOT_STR "Can't use both spider_use_consistent_snapshot = 1 and spider_internal_xa = 1"
#define ER_SPIDER_XA_LOCKED_NUM 12602
#define ER_SPIDER_XA_LOCKED_STR "This xid is now locked"
#define ER_SPIDER_XA_NOT_PREPARED_NUM 12603
#define ER_SPIDER_XA_NOT_PREPARED_STR "This xid is not prepared"
#define ER_SPIDER_XA_PREPARED_NUM 12604
#define ER_SPIDER_XA_PREPARED_STR "This xid is prepared"
#define ER_SPIDER_XA_EXISTS_NUM 12605
#define ER_SPIDER_XA_EXISTS_STR "This xid is already exist"
#define ER_SPIDER_XA_MEMBER_EXISTS_NUM 12606
#define ER_SPIDER_XA_MEMBER_EXISTS_STR "This xid member is already exist"
#define ER_SPIDER_XA_NOT_EXISTS_NUM 12607
#define ER_SPIDER_XA_NOT_EXISTS_STR "This xid is not exist"
#define ER_SPIDER_XA_MEMBER_NOT_EXISTS_NUM 12608
#define ER_SPIDER_XA_MEMBER_NOT_EXISTS_STR "This xid member is not exist"
#define ER_SPIDER_SYS_TABLE_VERSION_NUM 12609
#define ER_SPIDER_SYS_TABLE_VERSION_STR "System table %s is different version"
#define ER_SPIDER_WRONG_CHARACTER_IN_NAME_NUM 12611
#define ER_SPIDER_WRONG_CHARACTER_IN_NAME_STR "Wrong character in name string"
#define ER_SPIDER_LOW_MEM_READ_PREV_NUM 12621
#define ER_SPIDER_LOW_MEM_READ_PREV_STR "Can't use this operation at low mem read mode"
#define ER_SPIDER_ALTER_BEFORE_UNLOCK_NUM 12622
#define ER_SPIDER_ALTER_BEFORE_UNLOCK_STR "Can't use this operation before executing 'unlock tables'"
#define ER_SPIDER_REMOTE_SERVER_GONE_AWAY_NUM 12701
#define ER_SPIDER_REMOTE_SERVER_GONE_AWAY_STR "Remote MySQL server has gone away"
#define ER_SPIDER_REMOTE_SERVER_GONE_AWAY_LEN (sizeof(ER_SPIDER_REMOTE_SERVER_GONE_AWAY_STR) - 1)
#define ER_SPIDER_REMOTE_TABLE_NOT_FOUND_NUM 12702
#define ER_SPIDER_REMOTE_TABLE_NOT_FOUND_STR "Remote table '%s.%s' is not found"
#define ER_SPIDER_UDF_TMP_TABLE_NOT_FOUND_NUM 12703
#define ER_SPIDER_UDF_TMP_TABLE_NOT_FOUND_STR "Temporary table '%s.%s' is not found"
#define ER_SPIDER_UDF_COPY_TABLE_SRC_NOT_FOUND_NUM 12704
#define ER_SPIDER_UDF_COPY_TABLE_SRC_NOT_FOUND_STR "Source table is not found"
#define ER_SPIDER_UDF_COPY_TABLE_DST_NOT_FOUND_NUM 12705
#define ER_SPIDER_UDF_COPY_TABLE_DST_NOT_FOUND_STR "Destination table is not found"
#define ER_SPIDER_UDF_COPY_TABLE_SRC_NG_STATUS_NUM 12706
#define ER_SPIDER_UDF_COPY_TABLE_SRC_NG_STATUS_STR "Source table is NG status"
#define ER_SPIDER_UDF_COPY_TABLE_DST_NG_STATUS_NUM 12707
#define ER_SPIDER_UDF_COPY_TABLE_DST_NG_STATUS_STR "Destination table is NG status"
#define ER_SPIDER_UDF_CANT_OPEN_TABLE_NUM 12708
#define ER_SPIDER_UDF_CANT_OPEN_TABLE_STR "Can't open table %s.%s"
#define ER_SPIDER_UDF_COPY_TABLE_NEED_PK_NUM 12709
#define ER_SPIDER_UDF_COPY_TABLE_NEED_PK_STR "Table %s.%s needs PK for copying"
#define ER_SPIDER_INVALID_REMOTE_TABLE_INFO_NUM 12710
#define ER_SPIDER_INVALID_REMOTE_TABLE_INFO_STR "Invalid information from remote table '%s.%s'"
#define ER_SPIDER_HS_STR "Error from HS %d %s"
#define ER_SPIDER_HS_NUM 12711
#define ER_SPIDER_ORACLE_STR "Error from Oracle %d %d %s"
#define ER_SPIDER_ORACLE_NUM 12712
#define ER_SPIDER_ORACLE_ERR "Oracle error"
#define ER_SPIDER_COND_SKIP_NUM 12801

#define ER_SPIDER_UNKNOWN_NUM 12500
#define ER_SPIDER_UNKNOWN_STR "unknown"
#define ER_SPIDER_UNKNOWN_LEN (sizeof(ER_SPIDER_UNKNOWN_STR) - 1)
