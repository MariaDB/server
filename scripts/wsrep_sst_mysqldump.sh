#!/bin/sh -e
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

if test -z "$WSREP_SST_OPT_USER";  then err "USER cannot be nil";  exit $EINVAL; fi
if test -z "$WSREP_SST_OPT_HOST";  then err "HOST cannot be nil";  exit $EINVAL; fi
if test -z "$WSREP_SST_OPT_PORT";  then err "PORT cannot be nil";  exit $EINVAL; fi
if test -z "$WSREP_SST_OPT_LPORT"; then err "LPORT cannot be nil"; exit $EINVAL; fi
if test -z "$WSREP_SST_OPT_SOCKET";then err "SOCKET cannot be nil";exit $EINVAL; fi
if test -z "$WSREP_SST_OPT_GTID";  then err "GTID cannot be nil";  exit $EINVAL; fi

if local_ip $WSREP_SST_OPT_HOST && \
   [ "$WSREP_SST_OPT_PORT" = "$WSREP_SST_OPT_LPORT" ]
then
    wsrep_log_error \
    "destination address '$WSREP_SST_OPT_HOST:$WSREP_SST_OPT_PORT' matches source address."
    exit $EINVAL
fi

# Check client version
if ! mysql --version | grep 'Distrib 5.5' >/dev/null
then
    mysql --version >&2
    err "this operation requires MySQL client version 5.5.x"
    exit $EINVAL
fi

AUTH="-u$WSREP_SST_OPT_USER"
if test -n "$WSREP_SST_OPT_PSWD"; then AUTH="$AUTH -p$WSREP_SST_OPT_PSWD"; fi

STOP_WSREP="SET wsrep_on=OFF;"

# NOTE: we don't use --routines here because we're dumping mysql.proc table
MYSQLDUMP="mysqldump $AUTH -S$WSREP_SST_OPT_SOCKET \
--add-drop-database --add-drop-table --skip-add-locks --create-options \
--disable-keys --extended-insert --skip-lock-tables --quick --set-charset \
--skip-comments --flush-privileges --all-databases"

# mysqldump cannot restore CSV tables, fix this issue
CSV_TABLES_FIX="
set sql_mode='';

USE mysql;

SET @str = IF (@@have_csv = 'YES', 'CREATE TABLE IF NOT EXISTS general_log (event_time TIMESTAMP NOT NULL, user_host MEDIUMTEXT NOT NULL, thread_id INTEGER NOT NULL, server_id INTEGER UNSIGNED NOT NULL, command_type VARCHAR(64) NOT NULL,argument MEDIUMTEXT NOT NULL) engine=CSV CHARACTER SET utf8 comment=\"General log\"', 'SET @dummy = 0');

PREPARE stmt FROM @str;
EXECUTE stmt;
DROP PREPARE stmt;

SET @str = IF (@@have_csv = 'YES', 'CREATE TABLE IF NOT EXISTS slow_log (start_time TIMESTAMP NOT NULL, user_host MEDIUMTEXT NOT NULL, query_time TIME NOT NULL, lock_time TIME NOT NULL, rows_sent INTEGER NOT NULL, rows_examined INTEGER NOT NULL, db VARCHAR(512) NOT NULL, last_insert_id INTEGER NOT NULL, insert_id INTEGER NOT NULL, server_id INTEGER UNSIGNED NOT NULL, sql_text MEDIUMTEXT NOT NULL) engine=CSV CHARACTER SET utf8 comment=\"Slow log\"', 'SET @dummy = 0');

PREPARE stmt FROM @str;
EXECUTE stmt;
DROP PREPARE stmt;"

SET_START_POSITION="SET GLOBAL wsrep_start_position='$WSREP_SST_OPT_GTID';"

MYSQL="mysql $AUTH -h$WSREP_SST_OPT_HOST -P$WSREP_SST_OPT_PORT "\
"--disable-reconnect --connect_timeout=10"

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
    (echo $STOP_WSREP && $MYSQLDUMP && echo $CSV_TABLES_FIX \
    && echo $RESTORE_GENERAL_LOG && echo $RESTORE_SLOW_QUERY_LOG \
    && echo $SET_START_POSITION \
    || echo "SST failed to complete;") | $MYSQL
else
    wsrep_log_info "Bypassing state dump."
    echo $SET_START_POSITION | $MYSQL
fi

#
