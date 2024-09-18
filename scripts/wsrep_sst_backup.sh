#!/usr/bin/env bash

set -ue

# Copyright (C) 2017-2024 MariaDB
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

# This is a reference script for backup recovery state snapshot transfer.

. $(dirname "$0")/wsrep_sst_common

MAGIC_FILE="$DATA/backup_sst_complete"

wait_previous_sst

[ -f "$MAGIC_FILE" ] && rm -f "$MAGIC_FILE"

if [ "$WSREP_SST_OPT_ROLE" = 'donor' ]
then

    RC=0

    if [ $WSREP_SST_OPT_BYPASS -eq 0 ]; then

        FLUSHED="$DATA/tables_flushed"
        ERROR="$DATA/sst_error"

        [ -f "$FLUSHED" ] && rm -f "$FLUSHED"
        [ -f "$ERROR"   ] && rm -f "$ERROR"

        echo 'flush tables'

        # Wait for :
        # (a) Tables to be flushed, AND
        # (b) Cluster state ID & wsrep_gtid_domain_id to be written to the file, OR
        # (c) ERROR file, in case flush tables operation failed.

        while [ ! -r "$FLUSHED" ] || \
                ! grep -q -F ':' -- "$FLUSHED"
        do
            # Check whether ERROR file exists.
            if [ -f "$ERROR" ]; then
                # Flush tables operation failed.
                rm "$ERROR"
                exit 255
            fi
            sleep 0.2
        done

        STATE=$(cat "$FLUSHED")
        rm "$FLUSHED"

    else # BYPASS

        wsrep_log_info "Bypassing state dump."

        # Store donor's wsrep GTID (state ID) and wsrep_gtid_domain_id
        # (separated by a space).
        STATE="$WSREP_SST_OPT_GTID $WSREP_SST_OPT_GTID_DOMAIN_ID"

    fi

    echo 'continue' # now server can resume updating data

    echo "$STATE" > "$MAGIC_FILE"

    echo "done $STATE"

else # joiner

    wsrep_log_error "Unsupported role: '$WSREP_SST_OPT_ROLE'"
    exit 22 # EINVAL

fi

wsrep_log_info "$WSREP_METHOD $WSREP_TRANSFER_TYPE completed on $WSREP_SST_OPT_ROLE"
exit 0
