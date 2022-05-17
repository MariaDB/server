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

. $(dirname "$0")/wsrep_sst_common
wsrep_check_datadir

wsrep_check_programs rsync

cleanup_joiner()
{
    local failure=0

    wsrep_log_info "Joiner cleanup: rsync PID=$RSYNC_REAL_PID," \
                   "stunnel PID=$STUNNEL_REAL_PID"

    if [ -n "$STUNNEL" ]; then
        if cleanup_pid $STUNNEL_REAL_PID "$STUNNEL_PID" "$STUNNEL_CONF"; then
            if [ $RSYNC_REAL_PID -eq 0 ]; then
                if [ -r "$RSYNC_PID" ]; then
                    RSYNC_REAL_PID=$(cat "$RSYNC_PID" 2>/dev/null || :)
                    if [ -z "$RSYNC_REAL_PID" ]; then
                        RSYNC_REAL_PID=0
                    fi
                fi
            fi
        else
            wsrep_log_warning "stunnel cleanup failed."
            failure=1
        fi
    fi

    if [ $failure -eq 0 ]; then
        if cleanup_pid $RSYNC_REAL_PID "$RSYNC_PID" "$RSYNC_CONF"; then
            [ -f "$MAGIC_FILE" ] && rm -f "$MAGIC_FILE"
        else
            wsrep_log_warning "rsync cleanup failed."
        fi
    fi

    wsrep_log_info "Joiner cleanup done."

    if [ "$WSREP_SST_OPT_ROLE" = 'joiner' ]; then
        wsrep_cleanup_progress_file
    fi

    [ -f "$SST_PID" ] && rm -f "$SST_PID"
}

check_pid_and_port()
{
    local pid_file="$1"
    local pid=$2
    local addr="$3"
    local port="$4"

    local utils='rsync|stunnel'

    if ! check_port $pid "$port" "$utils"; then
        local port_info
        local busy=0

        if [ $lsof_available -ne 0 ]; then
            port_info=$(lsof -Pnl -i ":$port" 2>/dev/null | \
                        grep -F '(LISTEN)')
            echo "$port_info" | \
            grep -q -E "[[:space:]](\\*|\\[?::\\]?):$port[[:space:]]" && busy=1
        else
            local filter='([^[:space:]]+[[:space:]]+){4}[^[:space:]]+'
            if [ $sockstat_available -ne 0 ]; then
                local opts='-p'
                if [ "$OS" = 'FreeBSD' ]; then
                    # sockstat on FreeBSD requires the "-s" option
                    # to display the connection state:
                    opts='-sp'
                    # in addition, sockstat produces an additional column:
                    filter='([^[:space:]]+[[:space:]]+){5}[^[:space:]]+'
                fi
                port_info=$(sockstat "$opts" "$port" 2>/dev/null | \
                    grep -E '[[:space:]]LISTEN' | grep -o -E "$filter")
            else
                port_info=$(ss -nlpH "( sport = :$port )" 2>/dev/null | \
                    grep -F 'users:(' | grep -o -E "$filter")
            fi
            echo "$port_info" | \
            grep -q -E "[[:space:]](\\*|\\[?::\\]?):$port\$" && busy=1
        fi

        if [ $busy -eq 0 ]; then
            if ! echo "$port_info" | grep -qw -F "[$addr]:$port" && \
               ! echo "$port_info" | grep -qw -F -- "$addr:$port"
            then
                if ! ps -p $pid >/dev/null 2>&1; then
                    wsrep_log_error \
                        "rsync or stunnel daemon (PID: $pid)" \
                        "terminated unexpectedly."
                    exit 16 # EBUSY
                fi
                return 1
            fi
        fi

        if ! check_port $pid "$port" "$utils"; then
            wsrep_log_error "rsync or stunnel daemon port '$port'" \
                            "has been taken by another program"
            exit 16 # EBUSY
        fi
    fi

    check_pid "$pid_file" && [ $CHECK_PID -eq $pid ]
}

STUNNEL_CONF="$WSREP_SST_OPT_DATA/stunnel.conf"
STUNNEL_PID="$WSREP_SST_OPT_DATA/stunnel.pid"

MAGIC_FILE="$WSREP_SST_OPT_DATA/rsync_sst_complete"

BINLOG_TAR_FILE="$WSREP_SST_OPT_DATA/wsrep_sst_binlog.tar"
BINLOG_N_FILES=1

get_binlog

if [ -n "$WSREP_SST_OPT_BINLOG" ]; then
    BINLOG_DIRNAME=$(dirname "$WSREP_SST_OPT_BINLOG")
    BINLOG_FILENAME=$(basename "$WSREP_SST_OPT_BINLOG")
fi

# if no command line argument and INNODB_LOG_GROUP_HOME is not set,
# try to get it from my.cnf:
if [ -z "$INNODB_LOG_GROUP_HOME" ]; then
    INNODB_LOG_GROUP_HOME=$(parse_cnf '--mysqld' 'innodb-log-group-home-dir')
fi

OLD_PWD="$(pwd)"

WSREP_LOG_DIR="$INNODB_LOG_GROUP_HOME"

cd "$WSREP_SST_OPT_DATA"
if [ -n "$WSREP_LOG_DIR" ]; then
    # handle both relative and absolute paths
    [ ! -d "$WSREP_LOG_DIR" ] && mkdir -p "$WSREP_LOG_DIR"
    cd "$WSREP_LOG_DIR"
fi
WSREP_LOG_DIR=$(pwd -P)

cd "$OLD_PWD"

# if no command line argument and INNODB_DATA_HOME_DIR environment variable
# is not set, try to get it from my.cnf:
if [ -z "$INNODB_DATA_HOME_DIR" ]; then
    INNODB_DATA_HOME_DIR=$(parse_cnf '--mysqld' 'innodb-data-home-dir')
fi

cd "$WSREP_SST_OPT_DATA"
if [ -n "$INNODB_DATA_HOME_DIR" ]; then
    # handle both relative and absolute paths
    [ ! -d "$INNODB_DATA_HOME_DIR" ] && mkdir -p "$INNODB_DATA_HOME_DIR"
    cd "$INNODB_DATA_HOME_DIR"
fi
INNODB_DATA_HOME_DIR=$(pwd -P)

cd "$OLD_PWD"

# if no command line argument then try to get it from my.cnf:
if [ -z "$INNODB_UNDO_DIR" ]; then
    INNODB_UNDO_DIR=$(parse_cnf '--mysqld' 'innodb-undo-directory')
fi

cd "$WSREP_SST_OPT_DATA"
if [ -n "$INNODB_UNDO_DIR" ]; then
    # handle both relative and absolute paths
    [ ! -d "$INNODB_UNDO_DIR" ] && mkdir -p "$INNODB_UNDO_DIR"
    cd "$INNODB_UNDO_DIR"
fi
INNODB_UNDO_DIR=$(pwd -P)

cd "$OLD_PWD"

encgroups='--mysqld|sst'

check_server_ssl_config

SSTKEY="$tkey"
SSTCERT="$tpem"
SSTCA="$tcert"
SSTCAP="$tcap"

SSLMODE=$(parse_cnf "$encgroups" 'ssl-mode' | tr [:lower:] [:upper:])

if [ -z "$SSLMODE" ]; then
    # Implicit verification if CA is set and the SSL mode
    # is not specified by user:
    if [ -n "$SSTCA$SSTCAP" ]; then
        STUNNEL_BIN=$(commandex 'stunnel')
        if [ -n "$STUNNEL_BIN" ]; then
            SSLMODE='VERIFY_CA'
        fi
    # Require SSL by default if SSL key and cert are present:
    elif [ -n "$SSTKEY" -a -n "$SSTCERT" ]; then
        SSLMODE='REQUIRED'
    fi
else
    case "$SSLMODE" in
    'VERIFY_IDENTITY'|'VERIFY_CA'|'REQUIRED'|'DISABLED')
        ;;
    *)
        wsrep_log_error "Unrecognized ssl-mode option: '$SSLMODE'"
        exit 22 # EINVAL
        ;;
    esac
fi

if [ -n "$SSTKEY" -a -n "$SSTCERT" ]; then
    verify_cert_matches_key "$SSTCERT" "$SSTKEY"
fi

CAFILE_OPT=""
CAPATH_OPT=""
if [ -n "$SSTCA$SSTCAP" ]; then
    if [ -n "$SSTCA" ]; then
        CAFILE_OPT="CAfile = $SSTCA"
    fi
    if [ -n "$SSTCAP" ]; then
        CAPATH_OPT="CApath = $SSTCAP"
    fi
    if [ -n "$SSTCERT" ]; then
        verify_ca_matches_cert "$SSTCERT" "$SSTCA" "$SSTCAP"
    fi
fi

VERIFY_OPT=""
CHECK_OPT=""
CHECK_OPT_LOCAL=""
if [ "${SSLMODE#VERIFY}" != "$SSLMODE" ]; then
    if [ "$SSLMODE" = 'VERIFY_IDENTITY' ]; then
        VERIFY_OPT='verifyPeer = yes'
    else
        VERIFY_OPT='verifyChain = yes'
    fi
    if [ -z "$SSTCA$SSTCAP" ]; then
        wsrep_log_error "Can't have ssl-mode='$SSLMODE' without CA file or path"
        exit 22 # EINVAL
    fi
    if [ -n "$WSREP_SST_OPT_REMOTE_USER" ]; then
        CHECK_OPT="checkHost = $WSREP_SST_OPT_REMOTE_USER"
    elif [ "$WSREP_SST_OPT_ROLE" = 'donor' ]; then
        # check if the address is an ip-address (v4 or v6):
        if echo "$WSREP_SST_OPT_HOST_UNESCAPED" | \
           grep -q -E '^([0-9]+(\.[0-9]+){3}|[0-9a-fA-F]*(\:[0-9a-fA-F]*)+)$'
        then
            CHECK_OPT="checkIP = $WSREP_SST_OPT_HOST_UNESCAPED"
        else
            CHECK_OPT="checkHost = $WSREP_SST_OPT_HOST"
        fi
        if is_local_ip "$WSREP_SST_OPT_HOST_UNESCAPED"; then
            CHECK_OPT_LOCAL="checkHost = localhost"
        fi
    fi
fi

STUNNEL=""
if [ -n "$SSLMODE" -a "$SSLMODE" != 'DISABLED' ]; then
    if [ -z "${STUNNEL_BIN+x}" ]; then
        STUNNEL_BIN=$(commandex 'stunnel')
    fi
    if [ -n "$STUNNEL_BIN" ]; then
        wsrep_log_info "Using stunnel for SSL encryption: CA: '$SSTCA'," \
                       "CAPATH='$SSTCAP', ssl-mode='$SSLMODE'"
        STUNNEL="$STUNNEL_BIN $STUNNEL_CONF"
    fi
fi

readonly SECRET_TAG="secret"

if [ "$WSREP_SST_OPT_ROLE" = 'donor' ]
then

    [ -f "$MAGIC_FILE"      ] && rm -f "$MAGIC_FILE"
    [ -f "$BINLOG_TAR_FILE" ] && rm -f "$BINLOG_TAR_FILE"
    [ -f "$STUNNEL_PID"     ] && rm -f "$STUNNEL_PID"

    if [ -n "$STUNNEL" ]
    then
        cat << EOF > "$STUNNEL_CONF"
key = $SSTKEY
cert = $SSTCERT
${CAFILE_OPT}
${CAPATH_OPT}
foreground = yes
pid = $STUNNEL_PID
debug = warning
client = yes
connect = $WSREP_SST_OPT_HOST_UNESCAPED:$WSREP_SST_OPT_PORT
TIMEOUTclose = 0
${VERIFY_OPT}
${CHECK_OPT}
${CHECK_OPT_LOCAL}
EOF
    else
        [ -f "$STUNNEL_CONF" ] && rm -f "$STUNNEL_CONF"
    fi

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
                ! grep -q -F ':' '--' "$FLUSHED" >/dev/null 2>&1
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

        sync

        if [ -n "$WSREP_SST_OPT_BINLOG" -a -d "${BINLOG_DIRNAME:-}" ]
        then
            # Prepare binlog files
            cd "$BINLOG_DIRNAME"

            binlog_files_full=$(tail -n $BINLOG_N_FILES \
                                "$WSREP_SST_OPT_BINLOG_INDEX")
            binlog_files=""
            for file in $binlog_files_full; do
                binlog_file=$(basename "$file")
                binlog_files="$binlog_files${binlog_files:+ }'$binlog_file'"
            done

            if [ -n "$binlog_files" ]; then
                wsrep_log_info "Preparing binlog files for transfer:"
                eval tar -cvf "'$BINLOG_TAR_FILE'" $binlog_files >&2
            fi

            cd "$OLD_PWD"
        fi

        # Use deltaxfer only for WAN
        inv=$(basename "$0")
        WHOLE_FILE_OPT=""
        if [ "${inv%wsrep_sst_rsync_wan*}" = "$inv" ]; then
            WHOLE_FILE_OPT="--whole-file"
        fi

# Old filter - include everything except selected
# FILTER=(--exclude '*.err' --exclude '*.pid' --exclude '*.sock' \
#         --exclude '*.conf' --exclude core --exclude 'galera.*' \
#         --exclude grastate.txt --exclude '*.pem' \
#         --exclude '*.[0-9][0-9][0-9][0-9][0-9][0-9]' --exclude '*.index')

# New filter - exclude everything except dirs (schemas) and innodb files
FILTER="-f '- /lost+found'
        -f '- /.zfs'
        -f '- /.fseventsd'
        -f '- /.Trashes'
        -f '- /.pid'
        -f '- /.conf'
        -f '+ /wsrep_sst_binlog.tar'
        -f '- $INNODB_DATA_HOME_DIR/ib_lru_dump'
        -f '- $INNODB_DATA_HOME_DIR/ibdata*'
        -f '+ $INNODB_UNDO_DIR/undo*'
        -f '+ /*/'
        -f '- /*'"

        # first, the normal directories, so that we can detect
        # incompatible protocol:
        eval rsync ${STUNNEL:+"--rsh='$STUNNEL'"} \
              --owner --group --perms --links --specials \
              --ignore-times --inplace --dirs --delete --quiet \
              $WHOLE_FILE_OPT $FILTER "'$WSREP_SST_OPT_DATA/'" \
              "'rsync://$WSREP_SST_OPT_ADDR'" >&2 || RC=$?

        if [ $RC -ne 0 ]; then
            wsrep_log_error "rsync returned code $RC:"
            case $RC in
            12) RC=71  # EPROTO
                wsrep_log_error \
                    "rsync server on the other end has incompatible" \
                    "protocol. Make sure you have the same version of" \
                    "rsync on all nodes."
                ;;
            22) RC=12  # ENOMEM
                ;;
            *)  RC=255 # unknown error
                ;;
            esac
            exit $RC
        fi

        # Transfer InnoDB data files
        rsync ${STUNNEL:+--rsh="$STUNNEL"} \
              --owner --group --perms --links --specials \
              --ignore-times --inplace --dirs --delete --quiet \
              $WHOLE_FILE_OPT -f '+ /ibdata*' -f '+ /ib_lru_dump' \
              -f '- **' "$INNODB_DATA_HOME_DIR/" \
              "rsync://$WSREP_SST_OPT_ADDR-data_dir" >&2 || RC=$?

        if [ $RC -ne 0 ]; then
            wsrep_log_error "rsync innodb_data_home_dir returned code $RC:"
            exit 255 # unknown error
        fi

        # second, we transfer InnoDB and Aria log files
        rsync ${STUNNEL:+--rsh="$STUNNEL"} \
              --owner --group --perms --links --specials \
              --ignore-times --inplace --dirs --delete --quiet \
              $WHOLE_FILE_OPT -f '+ /ib_logfile[0-9]*' -f '+ /aria_log.*' \
              -f '+ /aria_log_control' -f '- **' "$WSREP_LOG_DIR/" \
              "rsync://$WSREP_SST_OPT_ADDR-log_dir" >&2 || RC=$?

        if [ $RC -ne 0 ]; then
            wsrep_log_error "rsync innodb_log_group_home_dir returned code $RC:"
            exit 255 # unknown error
        fi

        # then, we parallelize the transfer of database directories,
        # use '.' so that path concatenation works:

        cd "$WSREP_SST_OPT_DATA"

        backup_threads=$(parse_cnf '--mysqld|sst' 'backup-threads')
        if [ -z "$backup_threads" ]; then
            get_proc
            backup_threads=$nproc
        fi

        find . -maxdepth 1 -mindepth 1 -type d -not -name 'lost+found' \
             -not -name '.zfs' -print0 | xargs -I{} -0 -P $backup_threads \
             rsync ${STUNNEL:+--rsh="$STUNNEL"} \
             --owner --group --perms --links --specials --ignore-times \
             --inplace --recursive --delete --quiet $WHOLE_FILE_OPT \
             --exclude '*/ib_logfile*' --exclude '*/aria_log.*' \
             --exclude '*/aria_log_control' "$WSREP_SST_OPT_DATA/{}/" \
             "rsync://$WSREP_SST_OPT_ADDR/{}" >&2 || RC=$?

        cd "$OLD_PWD"

        if [ $RC -ne 0 ]; then
            wsrep_log_error "find/rsync returned code $RC:"
            exit 255 # unknown error
        fi

    else # BYPASS

        wsrep_log_info "Bypassing state dump."

        # Store donor's wsrep GTID (state ID) and wsrep_gtid_domain_id
        # (separated by a space).
        STATE="$WSREP_SST_OPT_GTID $WSREP_SST_OPT_GTID_DOMAIN_ID"

    fi

    echo 'continue' # now server can resume updating data

    echo "$STATE" > "$MAGIC_FILE"

    if [ -n "$WSREP_SST_OPT_REMOTE_PSWD" ]; then
        # Let joiner know that we know its secret
        echo "$SECRET_TAG $WSREP_SST_OPT_REMOTE_PSWD" >> "$MAGIC_FILE"
    fi

    rsync ${STUNNEL:+--rsh="$STUNNEL"} \
          --archive --quiet --checksum "$MAGIC_FILE" \
          "rsync://$WSREP_SST_OPT_ADDR" >&2 || RC=$?

    if [ $RC -ne 0 ]; then
        wsrep_log_error "rsync $MAGIC_FILE returned code $RC:"
        exit 255 # unknown error
    fi

    echo "done $STATE"

    if [ -n "$STUNNEL" ]; then
        [ -f "$STUNNEL_CONF" ] && rm -f "$STUNNEL_CONF"
        [ -f "$STUNNEL_PID"  ] && rm -f "$STUNNEL_PID"
    fi

elif [ "$WSREP_SST_OPT_ROLE" = 'joiner' ]
then
    check_sockets_utils

    SST_PID="$WSREP_SST_OPT_DATA/wsrep_sst.pid"

    # give some time for previous SST to complete:
    check_round=0
    while check_pid "$SST_PID" 0 'wsrep_sst_'; do
        wsrep_log_info "previous SST is not completed, waiting for it to exit"
        check_round=$(( check_round + 1 ))
        if [ $check_round -eq 10 ]; then
            wsrep_log_error "previous SST script still running."
            exit 114 # EALREADY
        fi
        sleep 1
    done

    echo $$ > "$SST_PID"

    # give some time for stunnel from the previous SST to complete:
    check_round=0
    while check_pid "$STUNNEL_PID" 1; do
        wsrep_log_info "Lingering stunnel daemon found at startup," \
                       "waiting for it to exit"
        check_round=$(( check_round + 1 ))
        if [ $check_round -eq 10 ]; then
            wsrep_log_error "stunnel daemon already running."
            exit 114 # EALREADY
        fi
        sleep 1
    done

    MODULE="${WSREP_SST_OPT_MODULE:-rsync_sst}"

    RSYNC_PID="$WSREP_SST_OPT_DATA/$MODULE.pid"
    RSYNC_CONF="$WSREP_SST_OPT_DATA/$MODULE.conf"

    # give some time for rsync from the previous SST to complete:
    check_round=0
    while check_pid "$RSYNC_PID" 1; do
        wsrep_log_info "Lingering rsync daemon found at startup," \
                       "waiting for it to exit"
        check_round=$(( check_round + 1 ))
        if [ $check_round -eq 10 ]; then
            wsrep_log_error "rsync daemon already running."
            exit 114 # EALREADY
        fi
        sleep 1
    done

    [ -f "$MAGIC_FILE"      ] && rm -f "$MAGIC_FILE"
    [ -f "$BINLOG_TAR_FILE" ] && rm -f "$BINLOG_TAR_FILE"

    [ -z "$STUNNEL" -a -f "$STUNNEL_CONF" ] && rm -f "$STUNNEL_CONF"

    ADDR="$WSREP_SST_OPT_HOST"
    RSYNC_PORT="$WSREP_SST_OPT_PORT"
    RSYNC_ADDR="$WSREP_SST_OPT_HOST"
    RSYNC_ADDR_UNESCAPED="$WSREP_SST_OPT_HOST_UNESCAPED"

    trap "exit 32" HUP PIPE
    trap "exit 3"  INT TERM ABRT
    trap cleanup_joiner EXIT

    touch "$SST_PROGRESS_FILE"

    if [ -n "${MYSQL_TMP_DIR:-}" ]; then
        SILENT="log file = $MYSQL_TMP_DIR/rsyncd.log"
    else
        SILENT=""
    fi

cat << EOF > "$RSYNC_CONF"
pid file = $RSYNC_PID
use chroot = no
read only = no
timeout = 300
$SILENT
[$MODULE]
    path = $WSREP_SST_OPT_DATA
    exclude = .zfs
[$MODULE-log_dir]
    path = $WSREP_LOG_DIR
[$MODULE-data_dir]
    path = $INNODB_DATA_HOME_DIR
EOF

#   rm -rf "$DATA/ib_logfile"* # we don't want old logs around

    # If the IP is local, listen only on it:
    if is_local_ip "$RSYNC_ADDR_UNESCAPED"
    then
        RSYNC_EXTRA_ARGS="--address $RSYNC_ADDR_UNESCAPED"
        STUNNEL_ACCEPT="$RSYNC_ADDR_UNESCAPED:$RSYNC_PORT"
    else
        # Not local, possibly a NAT, listen on all interfaces:
        RSYNC_EXTRA_ARGS=""
        STUNNEL_ACCEPT="$RSYNC_PORT"
        # Overwrite address with all:
        RSYNC_ADDR="*"
    fi

    if [ -z "$STUNNEL" ]; then
        rsync --daemon --no-detach --port "$RSYNC_PORT" \
              --config "$RSYNC_CONF" $RSYNC_EXTRA_ARGS &
        RSYNC_REAL_PID=$!
        TRANSFER_REAL_PID=$RSYNC_REAL_PID
        TRANSFER_PID="$RSYNC_PID"
    else
        # Let's check if the path to the config file contains a space?
        RSYNC_BIN=$(commandex 'rsync')
        if [ "${RSYNC_CONF#* }" = "$RSYNC_CONF" ]; then
            cat << EOF > "$STUNNEL_CONF"
key = $SSTKEY
cert = $SSTCERT
${CAFILE_OPT}
${CAPATH_OPT}
foreground = yes
pid = $STUNNEL_PID
debug = warning
client = no
${VERIFY_OPT}
${CHECK_OPT}
${CHECK_OPT_LOCAL}
[rsync]
accept = $STUNNEL_ACCEPT
exec = $RSYNC_BIN
execargs = rsync --server --daemon --config=$RSYNC_CONF .
EOF
        else
            # The path contains a space, so we will run it via
            # shell with "eval" command:
            export RSYNC_CMD="eval '$RSYNC_BIN' --server --daemon --config='$RSYNC_CONF' ."
            cat << EOF > "$STUNNEL_CONF"
key = $SSTKEY
cert = $SSTCERT
${CAFILE_OPT}
${CAPATH_OPT}
foreground = yes
pid = $STUNNEL_PID
debug = warning
client = no
${VERIFY_OPT}
${CHECK_OPT}
${CHECK_OPT_LOCAL}
[rsync]
accept = $STUNNEL_ACCEPT
exec = $SHELL
execargs = $SHELL -c \$RSYNC_CMD
EOF
        fi
        stunnel "$STUNNEL_CONF" &
        STUNNEL_REAL_PID=$!
        TRANSFER_REAL_PID=$STUNNEL_REAL_PID
        TRANSFER_PID="$STUNNEL_PID"
    fi

    if [ "${SSLMODE#VERIFY}" != "$SSLMODE" ]
    then # backward-incompatible behavior:
        CN=""
        if [ -n "$SSTCERT" ]
        then
            # find out my Common Name
            get_openssl
            if [ -z "$OPENSSL_BINARY" ]; then
                wsrep_log_error \
                    'openssl not found but it is required for authentication'
                exit 42
            fi
            CN=$("$OPENSSL_BINARY" x509 -noout -subject -in "$SSTCERT" | \
                 tr "," "\n" | grep -F 'CN =' | cut -d '=' -f2 | sed s/^\ // | \
                 sed s/\ %//)
        fi
        MY_SECRET="$(wsrep_gen_secret)"
        # Add authentication data to address
        ADDR="$CN:$MY_SECRET@$WSREP_SST_OPT_HOST"
    else
        MY_SECRET="" # for check down in recv_joiner()
    fi

    until check_pid_and_port "$TRANSFER_PID" $TRANSFER_REAL_PID \
          "$RSYNC_ADDR_UNESCAPED" "$RSYNC_PORT"
    do
        sleep 0.2
    done

    echo "ready $ADDR:$RSYNC_PORT/$MODULE"

    MYSQLD_PID="$WSREP_SST_OPT_PARENT"

    # wait for SST to complete by monitoring magic file
    while [ ! -r "$MAGIC_FILE" ] && check_pid "$TRANSFER_PID" && \
          ps -p $MYSQLD_PID >/dev/null 2>&1
    do
        sleep 1
    done

    if ! ps -p $MYSQLD_PID >/dev/null 2>&1
    then
        wsrep_log_error \
            "Parent mysqld process (PID: $MYSQLD_PID) terminated unexpectedly."
        kill -- -$MYSQLD_PID
        sleep 1
        exit 32
    fi

    if [ -n "$WSREP_SST_OPT_BINLOG" ]; then
        if [ -f "$BINLOG_TAR_FILE" ]; then
            cd "$BINLOG_DIRNAME"

            binlog_index="$WSREP_SST_OPT_BINLOG_INDEX"

            # Clean up old binlog files first
            rm -f "$BINLOG_FILENAME".[0-9]*
            [ -f "$binlog_index" ] && rm -f "$binlog_index"

            # Create a temporary file:
            tmpdir=$(parse_cnf '--mysqld|sst' 'tmpdir')
            if [ -z "$tmpdir" ]; then
               tmpfile="$(mktemp)"
            elif [ "$OS" = 'Linux' ]; then
               tmpfile=$(mktemp "--tmpdir=$tmpdir")
            else
               tmpfile=$(TMPDIR="$tmpdir"; mktemp)
            fi

            wsrep_log_info "Extracting binlog files:"
            if ! tar -xvf "$BINLOG_TAR_FILE" > "$tmpfile"; then
                wsrep_log_error "Error unpacking tar file with binlog files"
                rm -f "$tmpfile"
                exit 32
            fi

            # Rebuild binlog index:
            while read bin_file; do
                echo "$BINLOG_DIRNAME/$bin_file" >> "$binlog_index"
            done < "$tmpfile"
            rm -f "$tmpfile"

            cd "$OLD_PWD"
        fi
    fi

    if [ -r "$MAGIC_FILE" ]; then
        if [ -n "$MY_SECRET" ]; then
            # check donor supplied secret
            SECRET=$(grep -F -- "$SECRET_TAG " "$MAGIC_FILE" 2>/dev/null | \
                     cut -d ' ' -f 2)
            if [ "$SECRET" != "$MY_SECRET" ]; then
                wsrep_log_error "Donor does not know my secret!"
                wsrep_log_info "Donor: '$SECRET', my: '$MY_SECRET'"
                exit 32
            fi
            # remove secret from the magic file, and output
            # the UUID:seqno & wsrep_gtid_domain_id:
            grep -v -F -- "$SECRET_TAG " "$MAGIC_FILE"
        else
            # Output the UUID:seqno and wsrep_gtid_domain_id:
            cat "$MAGIC_FILE"
        fi
    else
        # this message should cause joiner to abort
        echo "rsync process ended without creating '$MAGIC_FILE'"
    fi

#   wsrep_cleanup_progress_file
#   cleanup_joiner
else
    wsrep_log_error "Unrecognized role: '$WSREP_SST_OPT_ROLE'"
    exit 22 # EINVAL
fi

[ -f "$BINLOG_TAR_FILE" ] && rm -f "$BINLOG_TAR_FILE"

exit 0
