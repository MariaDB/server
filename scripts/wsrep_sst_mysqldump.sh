#!/usr/bin/env bash

set -ue

# Copyright (C) 2009-2015 Codership Oy
# Copyright (C) 2017-2022 MariaDB
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
# MA  02110-1335  USA.

# This is a reference script for mysqldump-based state snapshot tansfer

. $(dirname "$0")/wsrep_sst_common

EINVAL=22

if test -z "$WSREP_SST_OPT_HOST";  then wsrep_log_error "HOST cannot be nil";  exit $EINVAL; fi
if test -z "$WSREP_SST_OPT_PORT";  then wsrep_log_error "PORT cannot be nil";  exit $EINVAL; fi
if test -z "$WSREP_SST_OPT_LPORT"; then wsrep_log_error "LPORT cannot be nil"; exit $EINVAL; fi
if test -z "$WSREP_SST_OPT_SOCKET";then wsrep_log_error "SOCKET cannot be nil";exit $EINVAL; fi
if test -z "$WSREP_SST_OPT_GTID";  then wsrep_log_error "GTID cannot be nil";  exit $EINVAL; fi

if is_local_ip "$WSREP_SST_OPT_HOST_UNESCAPED" && \
   [ "$WSREP_SST_OPT_PORT" = "$WSREP_SST_OPT_LPORT" ]
then
    wsrep_log_error \
    "destination address '$WSREP_SST_OPT_HOST:$WSREP_SST_OPT_PORT' matches source address."
    exit $EINVAL
fi

# Check client version
if ! $MYSQL_CLIENT --version | grep -q -E 'Distrib 10\.[1-9]'; then
    $MYSQL_CLIENT --version >&2
    wsrep_log_error "this operation requires MySQL client version 10.1 or newer"
    exit $EINVAL
fi

AUTH=""
usrst=0
if [ -n "$WSREP_SST_OPT_USER" ]; then
    AUTH="-u$WSREP_SST_OPT_USER"
    usrst=1
fi

# Refs https://github.com/codership/mysql-wsrep/issues/141
# Passing password in MYSQL_PWD environment variable is considered
# "extremely insecure" by MySQL Guidelines for Password Security
# (https://dev.mysql.com/doc/refman/5.6/en/password-security-user.html)
# that is even less secure than passing it on a command line! It is doubtful:
# the whole command line is easily observable by any unprivileged user via ps,
# whereas (at least on Linux) unprivileged user can't see process environment
# that he does not own. So while it may be not secure in the NSA sense of the
# word, it is arguably more secure than passing password on the command line.
if [ -n "$WSREP_SST_OPT_PSWD" ]; then
    export MYSQL_PWD="$WSREP_SST_OPT_PSWD"
elif [ $usrst -eq 1 ]; then
    # Empty password, used for testing, debugging etc.
    unset MYSQL_PWD
fi

STOP_WSREP='SET wsrep_on=OFF;'

# mysqldump cannot restore CSV tables, fix this issue
CSV_TABLES_FIX="
set sql_mode='';

USE mysql;

SET @cond = (SELECT (SUPPORT = 'YES' or SUPPORT = 'DEFAULT') FROM INFORMATION_SCHEMA.ENGINES WHERE ENGINE = 'csv');

SET @stmt = IF (@cond = '1', 'CREATE TABLE IF NOT EXISTS general_log ( event_time timestamp(6) NOT NULL DEFAULT CURRENT_TIMESTAMP(6) ON UPDATE CURRENT_TIMESTAMP(6), user_host mediumtext NOT NULL, thread_id bigint(21) unsigned NOT NULL, server_id int(10) unsigned NOT NULL, command_type varchar(64) NOT NULL, argument mediumtext NOT NULL) ENGINE=CSV DEFAULT CHARSET=utf8mb3 COMMENT=\"General log\"', 'SET @dummy = 0');

PREPARE stmt FROM @stmt;
EXECUTE stmt;
DROP PREPARE stmt;

SET @stmt = IF (@cond = '1', 'CREATE TABLE IF NOT EXISTS slow_log ( start_time timestamp(6) NOT NULL DEFAULT CURRENT_TIMESTAMP(6) ON UPDATE CURRENT_TIMESTAMP(6), user_host mediumtext NOT NULL, query_time time(6) NOT NULL, lock_time time(6) NOT NULL, rows_sent int(11) NOT NULL, rows_examined int(11) NOT NULL, db varchar(512) NOT NULL, last_insert_id int(11) NOT NULL, insert_id int(11) NOT NULL, server_id int(10) unsigned NOT NULL, sql_text mediumtext NOT NULL, thread_id bigint(21) unsigned NOT NULL) ENGINE=CSV DEFAULT CHARSET=utf8mb3 COMMENT=\"Slow log\"', 'SET @dummy = 0');

PREPARE stmt FROM @stmt;
EXECUTE stmt;
DROP PREPARE stmt;"

SET_START_POSITION="SET GLOBAL wsrep_start_position='$WSREP_SST_OPT_GTID';"

SET_WSREP_GTID_DOMAIN_ID=""
if [ -n "$WSREP_SST_OPT_GTID_DOMAIN_ID" ]; then
    SET_WSREP_GTID_DOMAIN_ID="
    SET @val = (SELECT GLOBAL_VALUE FROM INFORMATION_SCHEMA.SYSTEM_VARIABLES WHERE VARIABLE_NAME = 'WSREP_GTID_STRICT_MODE' AND GLOBAL_VALUE > 0);
    SET @stmt = IF (@val IS NOT NULL, 'SET GLOBAL WSREP_GTID_DOMAIN_ID=$WSREP_SST_OPT_GTID_DOMAIN_ID', 'SET @dummy = 0');
    PREPARE stmt FROM @stmt;
    EXECUTE stmt;
    DROP PREPARE stmt;"
fi

MYSQL="$MYSQL_CLIENT$WSREP_SST_OPT_CONF_UNQUOTED "\
"$AUTH -h$WSREP_SST_OPT_HOST_UNESCAPED "\
"-P$WSREP_SST_OPT_PORT --disable-reconnect --connect_timeout=10"

# Check if binary logging is enabled on the joiner node.
# Note: SELECT cannot be used at this point.
LOG_BIN=$(echo "set statement wsrep_sync_wait=0 for SHOW VARIABLES LIKE 'log_bin'" | $MYSQL | \
tail -1 | awk -F ' ' '{ print $2 }')

# Check the joiner node's server version.
SERVER_VERSION=$(echo "set statement wsrep_sync_wait=0 for SHOW VARIABLES LIKE 'version'" | $MYSQL | \
tail -1 | awk -F ' ' '{ print $2 }')

# Retrieve the donor's @@global.gtid_binlog_state.
GTID_BINLOG_STATE=$(echo "SHOW GLOBAL VARIABLES LIKE 'gtid_binlog_state'" | $MYSQL | \
tail -1 | awk -F ' ' '{ print $2 }')

RESET_MASTER=""
SET_GTID_BINLOG_STATE=""
SQL_LOG_BIN_OFF=""

# Safety check
if [ ${SERVER_VERSION%%.*} -gt 5 ]; then
    # If binary logging is enabled on the joiner node, we need to copy donor's
    # gtid_binlog_state to joiner. In order to do that, a RESET MASTER must be
    # executed to erase binary logs (if any). Binary logging should also be
    # turned off for the session so that gtid state does not get altered while
    # the dump gets replayed on joiner.
    if [ "$LOG_BIN" = 'ON' ]; then
        RESET_MASTER="SET GLOBAL wsrep_on=OFF; RESET MASTER; SET GLOBAL wsrep_on=ON;"
        SET_GTID_BINLOG_STATE="SET GLOBAL wsrep_on=OFF; SET @@global.gtid_binlog_state='$GTID_BINLOG_STATE'; SET GLOBAL wsrep_on=ON;"
        SQL_LOG_BIN_OFF="SET @@session.sql_log_bin=OFF;"
    fi
fi

# NOTE: we don't use --routines here because we're dumping mysql.proc table
MYSQLDUMP="$MYSQLDUMP$WSREP_SST_OPT_CONF_UNQUOTED $AUTH -S$WSREP_SST_OPT_SOCKET \
--add-drop-database --add-drop-table --skip-add-locks --create-options \
--disable-keys --extended-insert --skip-lock-tables --quick --set-charset \
--skip-comments --flush-privileges --all-databases --events"

if [ $WSREP_SST_OPT_BYPASS -eq 0 ]
then
    # need to disable logging when loading the dump
    # reason is that dump contains ALTER TABLE for log tables, and
    # this causes an error if logging is enabled
    GENERAL_LOG_OPT=$($MYSQL --skip-column-names -e "$STOP_WSREP SELECT @@GENERAL_LOG")
    SLOW_LOG_OPT=$($MYSQL --skip-column-names -e "$STOP_WSREP SELECT @@SLOW_QUERY_LOG")

    LOG_OFF="SET GLOBAL GENERAL_LOG=OFF; SET GLOBAL SLOW_QUERY_LOG=OFF;"

    # commands to restore log settings
    RESTORE_GENERAL_LOG="SET GLOBAL GENERAL_LOG=$GENERAL_LOG_OPT;"
    RESTORE_SLOW_QUERY_LOG="SET GLOBAL SLOW_QUERY_LOG=$SLOW_LOG_OPT;"

    (echo "$STOP_WSREP" && echo "$LOG_OFF" && echo "$RESET_MASTER" && \
     echo "$SET_GTID_BINLOG_STATE" && echo "$SQL_LOG_BIN_OFF" && \
     echo "$STOP_WSREP" && $MYSQLDUMP && echo "$CSV_TABLES_FIX" && \
     echo "$RESTORE_GENERAL_LOG" && echo "$RESTORE_SLOW_QUERY_LOG" && \
     echo "$SET_START_POSITION" && echo "$SET_WSREP_GTID_DOMAIN_ID" \
     || echo "SST failed to complete;") | $MYSQL
else
    wsrep_log_info "Bypassing state dump."
    echo "$SET_START_POSITION" | $MYSQL
fi

#
