#!/usr/bin/env bash

set -ue

# Copyright (C) 2017-2021 MariaDB
# Copyright (C) 2010-2014 Codership Oy
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

# This is a reference script for rsync-based state snapshot transfer

RSYNC_REAL_PID=0   # rsync process id
STUNNEL_REAL_PID=0 # stunnel process id

OS="$(uname)"
[ "$OS" = 'Darwin' ] && export -n LD_LIBRARY_PATH

# Setting the path for lsof on CentOS
export PATH="/usr/sbin:/sbin:$PATH"

. $(dirname "$0")/wsrep_sst_common

MAGIC_FILE="$WSREP_SST_OPT_DATA/backup_sst_complete"
rm -rf "$MAGIC_FILE"

WSREP_LOG_DIR=${WSREP_LOG_DIR:-""}
# if WSREP_LOG_DIR env. variable is not set, try to get it from my.cnf
if [ -z "$WSREP_LOG_DIR" ]; then
    WSREP_LOG_DIR=$(parse_cnf mysqld innodb-log-group-home-dir '')
fi

if [ -n "$WSREP_LOG_DIR" ]; then
    # handle both relative and absolute paths
    WSREP_LOG_DIR=$(cd $WSREP_SST_OPT_DATA; mkdir -p "$WSREP_LOG_DIR"; cd $WSREP_LOG_DIR; pwd -P)
else
    # default to datadir
    WSREP_LOG_DIR=$(cd $WSREP_SST_OPT_DATA; pwd -P)
fi

if [ "$WSREP_SST_OPT_ROLE" = 'donor' ]
then

    [ -f "$MAGIC_FILE"      ] && rm -f "$MAGIC_FILE"

    RC=0

    if [ $WSREP_SST_OPT_BYPASS -eq 0 ]; then

        FLUSHED="$WSREP_SST_OPT_DATA/tables_flushed"
        ERROR="$WSREP_SST_OPT_DATA/sst_error"

        [ -f "$FLUSHED" ] && rm -f "$FLUSHED"
        [ -f "$ERROR"   ] && rm -f "$ERROR"

        echo "flush tables"

        # Wait for :
        # (a) Tables to be flushed, AND
        # (b) Cluster state ID & wsrep_gtid_domain_id to be written to the file, OR
        # (c) ERROR file, in case flush tables operation failed.

        while [ ! -r "$FLUSHED" ] && \
                ! grep -q -F ':' -- "$FLUSHED" >/dev/null 2>&1
        do
            # Check whether ERROR file exists.
            if [ -f "$ERROR" ]; then
                # Flush tables operation failed.
                rm -f "$ERROR"
                exit 255
            fi
            sleep 0.2
        done

        STATE=$(cat "$FLUSHED")
        rm -f "$FLUSHED"


    else # BYPASS

        wsrep_log_info "Bypassing state dump."
    fi

    echo 'continue' # now server can resume updating data

    echo "$STATE" > "$MAGIC_FILE"

    echo "done $STATE"

else # joiner

    wsrep_log_error "Unsupported role: '$WSREP_SST_OPT_ROLE'"
    exit 22 # EINVAL

fi

exit 0
