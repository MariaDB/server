#!/bin/bash -e
# Copyright (C) 2009 Codership Oy
#
# This program is free software; you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation; version 2 of the License.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program; see the file COPYING. If not, write to the
# Free Software Foundation, Inc., 51 Franklin St, Fifth Floor, Boston
# MA  02110-1301  USA.

# This is a reference script for mysqldump-based state snapshot tansfer

# This variable is not used in mysqldump sst, so better initialize it
# to avoid shell's "parameter not set" message.
WSREP_SST_OPT_CONF=""

. $(dirname $0)/wsrep_sst_common

EINVAL=22

local_ip()
{
    PATH=$PATH:/usr/sbin:/usr/bin:/sbin:/bin

    [ "$1" = "127.0.0.1" ]      && return 0
    [ "$1" = "localhost" ]      && return 0
    [ "$1" = "$(hostname -s)" ] && return 0
    [ "$1" = "$(hostname -f)" ] && return 0
    [ "$1" = "$(hostname -d)" ] && return 0

    # Now if ip program is not found in the path, we can't return 0 since
    # it would block any address. Thankfully grep should fail in this case
    ip route get "$1" | grep local >/dev/null && return 0

    return 1
}

if test -z "$WSREP_SST_OPT_USER";  then wsrep_log_error "USER cannot be nil";  exit $EINVAL; fi
if test -z "$WSREP_SST_OPT_HOST";  then wsrep_log_error "HOST cannot be nil";  exit $EINVAL; fi
if test -z "$WSREP_SST_OPT_PORT";  then wsrep_log_error "PORT cannot be nil";  exit $EINVAL; fi
if test -z "$WSREP_SST_OPT_LPORT"; then wsrep_log_error "LPORT cannot be nil"; exit $EINVAL; fi
if test -z "$WSREP_SST_OPT_SOCKET";then wsrep_log_error "SOCKET cannot be nil";exit $EINVAL; fi
if test -z "$WSREP_SST_OPT_GTID";  then wsrep_log_error "GTID cannot be nil";  exit $EINVAL; fi

if local_ip $WSREP_SST_OPT_HOST && \
   [ "$WSREP_SST_OPT_PORT" = "$WSREP_SST_OPT_LPORT" ]
then
    wsrep_log_error \
    "destination address '$WSREP_SST_OPT_HOST:$WSREP_SST_OPT_PORT' matches source address."
    exit $EINVAL
fi

# Check client version
if ! mysql --version | grep 'Distrib 10.0' >/dev/null
then
    mysql --version >&2
    wsrep_log_error "this operation requires MySQL client version 10.0.x"
    exit $EINVAL
fi

# For Bug:1293798
if [ -z "$WSREP_SST_OPT_PSWD" -a -n "$WSREP_SST_OPT_AUTH" ]; then
    WSREP_SST_OPT_USER=$(echo $WSREP_SST_OPT_AUTH | cut -d: -f1)
    WSREP_SST_OPT_PSWD=$(echo $WSREP_SST_OPT_AUTH | cut -d: -f2)
fi
AUTH="-u$WSREP_SST_OPT_USER"
if test -n "$WSREP_SST_OPT_PSWD"; then AUTH="$AUTH -p$WSREP_SST_OPT_PSWD"; fi

STOP_WSREP="SET wsrep_on=OFF;"

# mysqldump cannot restore CSV tables, fix this issue
CSV_TABLES_FIX="
set sql_mode='';

USE mysql;

SET @cond = (SELECT (SUPPORT = 'YES' or SUPPORT = 'DEFAULT') FROM INFORMATION_SCHEMA.ENGINES WHERE ENGINE = 'csv');

SET @stmt = IF (@cond = '1', 'CREATE TABLE IF NOT EXISTS general_log ( event_time timestamp(6) NOT NULL DEFAULT CURRENT_TIMESTAMP(6) ON UPDATE CURRENT_TIMESTAMP(6), user_host mediumtext NOT NULL, thread_id bigint(21) unsigned NOT NULL, server_id int(10) unsigned NOT NULL, command_type varchar(64) NOT NULL, argument mediumtext NOT NULL) ENGINE=CSV DEFAULT CHARSET=utf8 COMMENT=\"General log\"', 'SET @dummy = 0');

PREPARE stmt FROM @stmt;
EXECUTE stmt;
DROP PREPARE stmt;

SET @stmt = IF (@cond = '1', 'CREATE TABLE IF NOT EXISTS slow_log ( start_time timestamp(6) NOT NULL DEFAULT CURRENT_TIMESTAMP(6) ON UPDATE CURRENT_TIMESTAMP(6), user_host mediumtext NOT NULL, query_time time(6) NOT NULL, lock_time time(6) NOT NULL, rows_sent int(11) NOT NULL, rows_examined int(11) NOT NULL, db varchar(512) NOT NULL, last_insert_id int(11) NOT NULL, insert_id int(11) NOT NULL, server_id int(10) unsigned NOT NULL, sql_text mediumtext NOT NULL, thread_id bigint(21) unsigned NOT NULL) ENGINE=CSV DEFAULT CHARSET=utf8 COMMENT=\"Slow log\"', 'SET @dummy = 0');

PREPARE stmt FROM @stmt;
EXECUTE stmt;
DROP PREPARE stmt;"

SET_START_POSITION="SET GLOBAL wsrep_start_position='$WSREP_SST_OPT_GTID';"

MYSQL="mysql $AUTH -h$WSREP_SST_OPT_HOST -P$WSREP_SST_OPT_PORT "\
"--disable-reconnect --connect_timeout=10"

RESET_MASTER=""
OPT_GALERA_SST_MODE=""
# Check if binary logging is enabled on the joiner node.
# Note: SELECT cannot be used at this point.
LOG_BIN=$(echo "SHOW VARIABLES LIKE 'log_bin'" | $MYSQL)
LOG_BIN=$(echo $LOG_BIN | awk -F ' ' '{ print $4 }')

# Check the joiner node's server version.
SERVER_VERSION=$(echo "SHOW VARIABLES LIKE 'version'" | $MYSQL)
SERVER_VERSION=$(echo $SERVER_VERSION | awk -F ' ' '{ print $4 }')

# Safety check
if echo $SERVER_VERSION | grep '^10.0' > /dev/null
then
  # Check if binary logging is enabled on the Joiner node. If it is enabled,
  # RESET MASTER needs to be executed on the Joiner node. Also, mysqldump
  # should be executed with additional 'galera-sst-mode' option so that it
  # adds a command to set gtid_binlog_state with that of Donor node in the
  # dump, to be executed on the Joiner.
  if [[ "$LOG_BIN" == 'ON' ]]; then
    # Reset master for 10.0 to clear gtid state.
    RESET_MASTER="RESET MASTER;"
    # Set the galera-sst-mode option for mysqldump.
    OPT_GALERA_SST_MODE="--galera-sst-mode"
  fi
fi

# NOTE: we don't use --routines here because we're dumping mysql.proc table
MYSQLDUMP="mysqldump $AUTH -S$WSREP_SST_OPT_SOCKET \
--add-drop-database --add-drop-table --skip-add-locks --create-options \
--disable-keys --extended-insert --skip-lock-tables --quick --set-charset \
--skip-comments --flush-privileges --all-databases $OPT_GALERA_SST_MODE"

# need to disable logging when loading the dump
# reason is that dump contains ALTER TABLE for log tables, and
# this causes an error if logging is enabled
GENERAL_LOG_OPT=`$MYSQL --skip-column-names -e"$STOP_WSREP SELECT @@GENERAL_LOG"`
SLOW_LOG_OPT=`$MYSQL --skip-column-names -e"$STOP_WSREP SELECT @@SLOW_QUERY_LOG"`
$MYSQL -e"$STOP_WSREP SET GLOBAL GENERAL_LOG=OFF"
$MYSQL -e"$STOP_WSREP SET GLOBAL SLOW_QUERY_LOG=OFF"

# commands to restore log settings
RESTORE_GENERAL_LOG="SET GLOBAL GENERAL_LOG=$GENERAL_LOG_OPT;"
RESTORE_SLOW_QUERY_LOG="SET GLOBAL SLOW_QUERY_LOG=$SLOW_LOG_OPT;"


if [ $WSREP_SST_OPT_BYPASS -eq 0 ]
then
    (echo $STOP_WSREP && echo $RESET_MASTER) | $MYSQL || true
    (echo $STOP_WSREP && $MYSQLDUMP && echo $CSV_TABLES_FIX \
    && echo $RESTORE_GENERAL_LOG && echo $RESTORE_SLOW_QUERY_LOG \
    && echo $SET_START_POSITION \
    || echo "SST failed to complete;") | $MYSQL
else
    wsrep_log_info "Bypassing state dump."
    echo $SET_START_POSITION | $MYSQL
fi

#
