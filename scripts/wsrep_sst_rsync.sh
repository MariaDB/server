#!/usr/bin/env bash

set -ue

# Copyright (C) 2017-2024 MariaDB
# Copyright (C) 2010-2022 Codership Oy
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

# This is a reference script for rsync-based state snapshot transfer.

. $(dirname "$0")/wsrep_sst_common

wsrep_check_programs rsync

RSYNC_REAL_PID=0   # rsync process id
STUNNEL_REAL_PID=0 # stunnel process id

MODULE="${WSREP_SST_OPT_MODULE:-rsync_sst}"

RSYNC_PID="$DATA/$MODULE.pid"
RSYNC_CONF="$DATA/$MODULE.conf"

STUNNEL_CONF="$DATA/stunnel.conf"
STUNNEL_PID="$DATA/stunnel.pid"

MAGIC_FILE="$DATA/rsync_sst_complete"

cleanup_joiner()
{
    # Since this is invoked just after exit NNN
    local estatus=$?
    if [ $estatus -ne 0 ]; then
        wsrep_log_error "Cleanup after exit with status: $estatus"
    elif [ -z "${coords:-}" ]; then
        estatus=32
        wsrep_log_error "Failed to get current position"
    fi

    local failure=0

    [ "$(pwd)" != "$OLD_PWD" ] && cd "$OLD_PWD"

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
            [ -f "$MAGIC_FILE" ] && rm -f "$MAGIC_FILE" || :
            [ -f "$BINLOG_TAR_FILE" ] && rm -f "$BINLOG_TAR_FILE" || :
        else
            wsrep_log_warning "rsync cleanup failed."
        fi
    fi

    if [ "$WSREP_SST_OPT_ROLE" = 'joiner' ]; then
        wsrep_cleanup_progress_file
    fi

    [ -f "$SST_PID" ] && rm -f "$SST_PID" || :

    wsrep_log_info "Joiner cleanup done."

    exit $estatus
}

check_pid_and_port()
{
    local pid_file="$1"
    local pid=$2
    local addr="$3"
    local port="$4"

    local utils='rsync|stunnel'

    local port_info
    local final

    if ! check_port $pid "$port" "$utils"; then
        if [ $raw_socket_check -ne 0 ]; then
            return 1
        elif [ $ss_available -ne 0 -o $sockstat_available -ne 0 ]; then
            if [ $ss_available -ne 0 ]; then
                port_info=$($socket_utility $ss_opts -t "( sport = :$port )" 2>/dev/null | \
                    grep -E '[[:space:]]users:[[:space:]]?\(' | \
                    grep -o -E "([^[:space:]]+[[:space:]]+){4}[^[:space:]]+" || :)
            else
                if [ $sockstat_available -gt 1 ]; then
                    # The sockstat command on FreeBSD does not return
                    # the connection state without special option, but
                    # it supports filtering by connection state.
                    # Additionally, the sockstat utility on FreeBSD
                    # produces an one extra column:
                    port_info=$($socket_utility $sockstat_opts "$port" 2>/dev/null | \
                        grep -o -E "([^[:space:]]+[[:space:]]+){5}[^[:space:]]+" || :)
                else
                    port_info=$($socket_utility $sockstat_opts "$port" 2>/dev/null | \
                        grep -E '[[:space:]]LISTEN([[:space:]]|$)' | \
                        grep -o -E "([^[:space:]]+[[:space:]]+){4}[^[:space:]]+" || :)
                fi
            fi
            final='$'
        else
            port_info=$($socket_utility $lsof_opts -i ":$port" 2>/dev/null | \
                grep -w -F '(LISTEN)' || :)
            final='[[:space:]]'
        fi

        local busy=0
        if [ -n "$port_info" ]; then
            local address='(\*|[0-9a-fA-F]*(:[0-9a-fA-F]*){1,7}|[0-9]+(\.[0-9]+){3})'
            local filter="[[:space:]]($address|\\[$address\\])(%[^:]+)?:$port$final"
            echo "$port_info" | grep -q -E "$filter" && busy=1
        fi

        if [ $busy -eq 0 ]; then
            if ! ps -p $pid >/dev/null 2>&1; then
                wsrep_log_error \
                    "the rsync or stunnel daemon (PID: $pid)" \
                    "terminated unexpectedly."
                exit 16 # EBUSY
            fi
            return 1
        fi

        local rc=0
        check_port $pid "$port" "$utils" || rc=$?
        if [ $rc -eq 16 ]; then
            # We will ignore the return code EBUSY, which indicates
            # a failed attempt to run the utility for retrieving
            # socket information (on some systems):
            return 1
        elif [ $rc -ne 0 ]; then
            wsrep_log_error "rsync or stunnel daemon port '$port'" \
                            "has been taken by another program"
            exit 16 # EBUSY
        fi
    fi

    if [ $raw_socket_check -ne 0 ]; then
	return 0
    fi
    check_pid "$pid_file" && [ "$CHECK_PID" -eq "$pid" ]
}

get_binlog

if [ -n "$WSREP_SST_OPT_BINLOG" ]; then
    binlog_dir=$(dirname "$WSREP_SST_OPT_BINLOG")
    binlog_base=$(basename "$WSREP_SST_OPT_BINLOG")
fi

BINLOG_TAR_FILE="$DATA_DIR/wsrep_sst_binlog.tar"

ar_log_dir="$DATA_DIR"
ib_log_dir="$DATA_DIR"
ib_home_dir="$DATA_DIR"
ib_undo_dir="$DATA_DIR"

encgroups='--mysqld|sst'

check_server_ssl_config

SSTKEY="$tkey"
SSTCERT="$tpem"
SSTCA="$tcert"
SSTCAP="$tcap"

SSLMODE=$(parse_cnf "$encgroups" 'ssl-mode' | tr '[[:lower:]]' '[[:upper:]]')

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
           grep -q -E '^([0-9]+(\.[0-9]+){3}|[0-9a-fA-F]*(:[0-9a-fA-F]*){1,7})$'
        then
            CHECK_OPT="checkIP = $WSREP_SST_OPT_HOST_UNESCAPED"
        else
            CHECK_OPT="checkHost = $WSREP_SST_OPT_HOST"
        fi
        if is_local_ip "$WSREP_SST_OPT_HOST_UNESCAPED"; then
            CHECK_OPT_LOCAL='checkHost = localhost'
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

readonly SECRET_TAG='secret'
readonly BYPASS_TAG='bypass'

wait_previous_sst

# give some time for stunnel from the previous SST to complete:
check_round=0
while check_pid "$STUNNEL_PID" 1 "$STUNNEL_CONF"; do
    wsrep_log_info "Lingering stunnel daemon found at startup," \
                   "waiting for it to exit"
    check_round=$(( check_round+1 ))
    if [ $check_round -eq 30 ]; then
        wsrep_log_error "stunnel daemon still running..."
        exit 114 # EALREADY
    fi
    sleep 1
done

# give some time for rsync from the previous SST to complete:
check_round=0
while check_pid "$RSYNC_PID" 1 "$RSYNC_CONF"; do
    wsrep_log_info "Lingering rsync daemon found at startup," \
                   "waiting for it to exit"
    check_round=$(( check_round+1 ))
    if [ $check_round -eq 30 ]; then
        wsrep_log_error "rsync daemon still running..."
        exit 114 # EALREADY
    fi
    sleep 1
done

[ -f "$MAGIC_FILE" ]      && rm -f "$MAGIC_FILE"
[ -f "$BINLOG_TAR_FILE" ] && rm -f "$BINLOG_TAR_FILE"

RC=0

if [ "$WSREP_SST_OPT_ROLE" = 'donor' ]; then

    if [ -n "$STUNNEL" ]; then
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
    fi

    if [ $WSREP_SST_OPT_BYPASS -eq 0 ]; then

        FLUSHED="$DATA/tables_flushed"
        ERROR="$DATA/sst_error"

        [ -f "$FLUSHED" ] && rm -f "$FLUSHED"
        [ -f "$ERROR" ]   && rm -f "$ERROR"

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

        sync

        wsrep_log_info "Tables flushed"

        if [ -n "$WSREP_SST_OPT_BINLOG" ]; then
            # Change the directory to binlog base (if possible):
            cd "$DATA"
            # Let's check the existence of the file with the index:
            if [ -f "$WSREP_SST_OPT_BINLOG_INDEX" ]; then
                # Let's read the binlog index:
                max_binlogs=$(parse_cnf "$encgroups" 'sst-max-binlogs' 1)
                if [ $max_binlogs -ge 0 ]; then
                    binlog_files=""
                    if [ $max_binlogs -gt 0 ]; then
                        binlog_files=$(tail -n $max_binlogs \
                                       "$WSREP_SST_OPT_BINLOG_INDEX")
                    fi
                else
                    binlog_files=$(cat "$WSREP_SST_OPT_BINLOG_INDEX")
                fi
                if [ -n "$binlog_files" ]; then
                    # Preparing binlog files for transfer:
                    wsrep_log_info "Preparing binlog files for transfer:"
                    tar_type=0
                    if tar --help 2>/dev/null | grep -qw -F -- '--transform'; then
                        tar_type=1
                    elif tar --version 2>/dev/null | grep -qw -E '^bsdtar'; then
                        tar_type=2
                    fi
                    if [ $tar_type -eq 2 ]; then
                        echo "$binlog_files" >&2
                    fi
                    if [ $tar_type -ne 0 ]; then
                        # Preparing list of the binlog file names:
                        echo "$binlog_files" | {
                            binlogs=""
                            while read bin_file || [ -n "$bin_file" ]; do
                                [ ! -f "$bin_file" ] && continue
                                if [ -n "$BASH_VERSION" ]; then
                                    first="${bin_file:0:1}"
                                else
                                    first=$(echo "$bin_file" | cut -c1)
                                fi
                                if [ "$first" = '-' -o "$first" = '@' ]; then
                                    bin_file="./$bin_file"
                                fi
                                binlogs="$binlogs${binlogs:+ }'$bin_file'"
                            done
                            if [ -n "$binlogs" ]; then
                                if [ $tar_type -eq 1 ]; then
                                    tar_options="--transform='s/^.*\///g'"
                                else
                                    # bsdtar handles backslash incorrectly:
                                    tar_options="-s '?^.*/??g'"
                                fi
                                eval tar -P $tar_options \
                                         -cvf "'$BINLOG_TAR_FILE'" $binlogs >&2
                            fi
                        }
                    else
                        tar_options='-cvf'
                        echo "$binlog_files" | \
                        while read bin_file || [ -n "$bin_file" ]; do
                            [ ! -f "$bin_file" ] && continue
                            bin_dir=$(dirname "$bin_file")
                            bin_base=$(basename "$bin_file")
                            if [ -n "$BASH_VERSION" ]; then
                                first="${bin_base:0:1}"
                            else
                                first=$(echo "$bin_base" | cut -c1)
                            fi
                            if [ "$first" = '-' -o "$first" = '@' ]; then
                                bin_base="./$bin_base"
                            fi
                            if [ -n "$bin_dir" -a "$bin_dir" != '.' -a \
                                 "$bin_dir" != "$DATA_DIR" ]
                            then
                                tar $tar_options "$BINLOG_TAR_FILE" \
                                    -C "$bin_dir" "$bin_base" >&2
                            else
                                tar $tar_options "$BINLOG_TAR_FILE" \
                                    "$bin_base" >&2
                            fi
                            tar_options='-rvf'
                        done
                    fi
                fi
            fi
            cd "$OLD_PWD"
        fi

        # Use deltaxfer only for WAN:
        WHOLE_FILE_OPT=""
        if [ "${WSREP_METHOD%_wan}" = "$WSREP_METHOD" ]; then
            WHOLE_FILE_OPT='--whole-file'
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
        -f '- /.snapshot/'
        -f '+ /wsrep_sst_binlog.tar'
        -f '- $ib_home_dir/ib_lru_dump'
        -f '- $ib_home_dir/ibdata*'
        -f '- $ib_undo_dir/undo*'
        -f '- $ib_log_dir/ib_logfile[0-9]*'
        -f '- $ar_log_dir/aria_log_control'
        -f '- $ar_log_dir/aria_log.*'
        -f '+ /*/'
        -f '- /*'"

        # first, the normal directories, so that we can detect
        # incompatible protocol:
        eval rsync ${STUNNEL:+"--rsh='$STUNNEL'"} \
              --owner --group --perms --links --specials \
              --ignore-times --inplace --dirs --delete --quiet \
              $WHOLE_FILE_OPT $FILTER "'$DATA/'" \
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

        wsrep_log_info "Transfer of normal directories done"

        if [ -d "$ib_home_dir" ]; then

            # Transfer InnoDB data files
            rsync ${STUNNEL:+--rsh="$STUNNEL"} \
                  --owner --group --perms --links --specials \
                  --ignore-times --inplace --dirs --delete --quiet \
                  $WHOLE_FILE_OPT -f '+ /ibdata*' -f '+ /ib_lru_dump' \
                  -f '- **' "$ib_home_dir/" \
                  "rsync://$WSREP_SST_OPT_ADDR-data_dir" >&2 || RC=$?

            if [ $RC -ne 0 ]; then
                wsrep_log_error "rsync innodb_data_home_dir returned code $RC:"
                exit 255 # unknown error
            fi

            wsrep_log_info "Transfer of InnoDB data files done"

        fi

        if [ -d "$ib_log_dir" ]; then

            # second, we transfer the InnoDB log file
            rsync ${STUNNEL:+--rsh="$STUNNEL"} \
                  --owner --group --perms --links --specials \
                  --ignore-times --inplace --dirs --delete --quiet \
                  $WHOLE_FILE_OPT -f '+ /ib_logfile0' \
                  -f '- **' "$ib_log_dir/" \
                  "rsync://$WSREP_SST_OPT_ADDR-log_dir" >&2 || RC=$?

            if [ $RC -ne 0 ]; then
                wsrep_log_error "rsync innodb_log_group_home_dir returned code $RC:"
                exit 255 # unknown error
            fi

            wsrep_log_info "Transfer of InnoDB log files done"

        fi

        if [ "$ib_undo_dir" ]; then

            # third, we transfer InnoDB undo logs
            rsync ${STUNNEL:+--rsh="$STUNNEL"} \
                  --owner --group --perms --links --specials \
                  --ignore-times --inplace --dirs --delete --quiet \
                  $WHOLE_FILE_OPT -f '+ /undo*' \
                  -f '- **' "$ib_undo_dir/" \
                  "rsync://$WSREP_SST_OPT_ADDR-undo_dir" >&2 || RC=$?

            if [ $RC -ne 0 ]; then
                wsrep_log_error "rsync innodb_undo_dir returned code $RC:"
                exit 255 # unknown error
            fi

            wsrep_log_info "Transfer of InnoDB undo logs done"

        fi

        if [ "$ar_log_dir" ]; then

            # fourth, we transfer Aria logs
            rsync ${STUNNEL:+--rsh="$STUNNEL"} \
                  --owner --group --perms --links --specials \
                  --ignore-times --inplace --dirs --delete --quiet \
                  $WHOLE_FILE_OPT -f '+ /aria_log_control' -f '+ /aria_log.*' \
                  -f '- **' "$ar_log_dir/" \
                  "rsync://$WSREP_SST_OPT_ADDR-aria_log" >&2 || RC=$?

            if [ $RC -ne 0 ]; then
                wsrep_log_error "rsync aria_log_dir_path returned code $RC:"
                exit 255 # unknown error
            fi

            wsrep_log_info "Transfer of Aria logs done"

        fi

        # then, we parallelize the transfer of database directories,
        # use '.' so that path concatenation works:

        backup_threads=$(parse_cnf '--mysqld|sst' 'backup-threads')
        if [ -z "$backup_threads" ]; then
            get_proc
            backup_threads=$nproc
        fi

        cd "$DATA"

        findopt='-L'
        [ "$OS" = 'FreeBSD' ] && findopt="$findopt -E"

        find $findopt . -maxdepth 1 -mindepth 1 -type d -not -name 'lost+found' \
             -not -name '.zfs' -not -name .snapshot -print0 \
             | xargs -I{} -0 -P $backup_threads \
             rsync ${STUNNEL:+--rsh="$STUNNEL"} \
             --owner --group --perms --links --specials --ignore-times \
             --inplace --recursive --delete --quiet $WHOLE_FILE_OPT \
             -f '- $ib_home_dir/ib_lru_dump' \
             -f '- $ib_home_dir/ibdata*' \
             -f '- $ib_undo_dir/undo*' \
             -f '- $ib_log_dir/ib_logfile[0-9]*' \
             -f '- $ar_log_dir/aria_log_control' \
             -f '- $ar_log_dir/aria_log.*' \
             "$DATA/{}/" \
             "rsync://$WSREP_SST_OPT_ADDR/{}" >&2 || RC=$?

        cd "$OLD_PWD"

        if [ $RC -ne 0 ]; then
            wsrep_log_error "find/rsync returned code $RC:"
            exit 255 # unknown error
        fi

        wsrep_log_info "Transfer of data done"

        [ -f "$BINLOG_TAR_FILE" ] && rm "$BINLOG_TAR_FILE"

    else # BYPASS

        wsrep_log_info "Bypassing state dump."

        # Store donor's wsrep GTID (state ID) and wsrep_gtid_domain_id
        # (separated by a space).
        STATE="$WSREP_SST_OPT_GTID $WSREP_SST_OPT_GTID_DOMAIN_ID"

    fi

    wsrep_log_info "Sending continue to donor"
    echo 'continue' # now server can resume updating data

    echo "$STATE" > "$MAGIC_FILE"

    if [ -n "$WSREP_SST_OPT_REMOTE_PSWD" ]; then
        # Let joiner know that we know its secret
        echo "$SECRET_TAG $WSREP_SST_OPT_REMOTE_PSWD" >> "$MAGIC_FILE"
    fi

    if [ $WSREP_SST_OPT_BYPASS -ne 0 ]; then
        echo "$BYPASS_TAG" >> "$MAGIC_FILE"
    fi

    rsync ${STUNNEL:+--rsh="$STUNNEL"} \
          --archive --quiet --checksum "$MAGIC_FILE" \
          "rsync://$WSREP_SST_OPT_ADDR" >&2 || RC=$?

    rm "$MAGIC_FILE"

    if [ $RC -ne 0 ]; then
        wsrep_log_error "rsync $MAGIC_FILE returned code $RC:"
        exit 255 # unknown error
    fi

    echo "done $STATE"

    if [ -n "$STUNNEL" ]; then
        rm "$STUNNEL_CONF"
        [ -f "$STUNNEL_PID" ] && rm "$STUNNEL_PID"
    fi

else # joiner

    check_sockets_utils

    create_dirs

    ADDR="$WSREP_SST_OPT_HOST"
    RSYNC_PORT="$WSREP_SST_OPT_PORT"
    RSYNC_ADDR="$WSREP_SST_OPT_HOST"
    RSYNC_ADDR_UNESCAPED="$WSREP_SST_OPT_HOST_UNESCAPED"

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
    path = $DATA
    exclude = .zfs
[$MODULE-log_dir]
    path = $ib_log_dir
[$MODULE-data_dir]
    path = $ib_home_dir
[$MODULE-undo_dir]
    path = $ib_undo_dir
[$MODULE-aria_log]
    path = $ar_log_dir
EOF

    # If the IP is local, listen only on it:
    if is_local_ip "$RSYNC_ADDR_UNESCAPED"; then
        RSYNC_EXTRA_ARGS="--address $RSYNC_ADDR_UNESCAPED"
        STUNNEL_ACCEPT="$RSYNC_ADDR_UNESCAPED:$RSYNC_PORT"
    else
        # Not local, possibly a NAT, listen on all interfaces:
        RSYNC_EXTRA_ARGS=""
        STUNNEL_ACCEPT="$RSYNC_PORT"
        # Overwrite address with all:
        RSYNC_ADDR='*'
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

    if [ "${SSLMODE#VERIFY}" != "$SSLMODE" ]; then
        # backward-incompatible behavior:
        CN=""
        if [ -n "$SSTCERT" ]; then
            CN=$(openssl_getCN "$SSTCERT")
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

    # wait for SST to complete by monitoring magic file
    while [ ! -r "$MAGIC_FILE" ] && check_pid "$TRANSFER_PID" && \
          ps -p $WSREP_SST_OPT_PARENT >/dev/null 2>&1
    do
        sleep 1
    done

    if ! ps -p $WSREP_SST_OPT_PARENT >/dev/null 2>&1; then
        wsrep_log_error \
            "Parent mysqld process (PID: $WSREP_SST_OPT_PARENT)" \
            "terminated unexpectedly."
        kill -- -$WSREP_SST_OPT_PARENT
        sleep 1
        exit 32
    fi

    if [ ! -r "$MAGIC_FILE" ]; then
        # This message should cause joiner to abort:
        wsrep_log_info "rsync process ended without creating" \
                       "magic file ($MAGIC_FILE)"
        exit 32
    fi

    if [ -n "$MY_SECRET" ]; then
        # Check donor supplied secret:
        SECRET=$(grep -m1 -E "^$SECRET_TAG[[:space:]]" "$MAGIC_FILE" || :)
        SECRET=$(trim_string "${SECRET#$SECRET_TAG}")
        if [ "$SECRET" != "$MY_SECRET" ]; then
            wsrep_log_error "Donor does not know my secret!"
            wsrep_log_info "Donor: '$SECRET', my: '$MY_SECRET'"
            exit 32
        fi
    fi

    if [ $WSREP_SST_OPT_BYPASS -eq 0 ]; then
        if grep -m1 -qE "^$BYPASS_TAG([[:space:]]+.*)?\$" "$MAGIC_FILE"; then
            readonly WSREP_SST_OPT_BYPASS=1
            readonly WSREP_TRANSFER_TYPE='IST'
        fi
    fi

    binlog_tar_present=0
    if [ -f "$BINLOG_TAR_FILE" ]; then
        binlog_tar_present=1
        if [ $WSREP_SST_OPT_BYPASS -ne 0 ]; then
            wsrep_log_warning "tar with binlogs transferred in the IST mode"
        fi
    fi

    if [ $WSREP_SST_OPT_BYPASS -eq 0 -a -n "$WSREP_SST_OPT_BINLOG" ]; then
        # If it is SST (not an IST) or tar with binlogs is present
        # among the transferred files, then we need to remove the
        # old binlogs:
        cd "$DATA"
        # Clean up the old binlog files and index:
        binlog_index="$WSREP_SST_OPT_BINLOG_INDEX"
        if [ -f "$binlog_index" ]; then
            while read bin_file || [ -n "$bin_file" ]; do
                rm -f "$bin_file" || :
            done < "$binlog_index"
            rm "$binlog_index"
        fi
        binlog_cd=0
        # Change the directory to binlog base (if possible):
        if [ -n "$binlog_dir" -a "$binlog_dir" != '.' -a \
             "$binlog_dir" != "$DATA_DIR" -a -d "$binlog_dir" ]
        then
            binlog_cd=1
            cd "$binlog_dir"
        fi
        # Clean up unindexed binlog files:
        rm -f "$binlog_base".[0-9]* || :
        [ $binlog_cd -ne 0 ] && cd "$DATA_DIR"
        if [ $binlog_tar_present -ne 0 ]; then
            # Create a temporary file:
            tmpdir=$(parse_cnf '--mysqld|sst' 'tmpdir')
            if [ -z "$tmpdir" ]; then
               tmpfile="$(mktemp)"
            elif [ "$OS" = 'Linux' ]; then
               tmpfile=$(mktemp "--tmpdir=$tmpdir")
            else
               tmpfile=$(TMPDIR="$tmpdir"; mktemp)
            fi
            index_dir=$(dirname "$binlog_index");
            if [ -n "$index_dir" -a "$index_dir" != '.' -a \
                 "$index_dir" != "$DATA_DIR" ]
            then
                [ ! -d "$index_dir" ] && mkdir -p "$index_dir"
            fi
            binlog_cd=0
            if [ -n "$binlog_dir" -a "$binlog_dir" != '.' -a \
                 "$binlog_dir" != "$DATA_DIR" ]
            then
                [ ! -d "$binlog_dir" ] && mkdir -p "$binlog_dir"
                binlog_cd=1
                cd "$binlog_dir"
            fi
            # Extracting binlog files:
            wsrep_log_info "Extracting binlog files:"
            if tar --version 2>/dev/null | grep -qw -E '^bsdtar'; then
                tar -tf "$BINLOG_TAR_FILE" > "$tmpfile" && \
                tar -xvf "$BINLOG_TAR_FILE" > /dev/null || RC=$?
            else
                tar -xvf "$BINLOG_TAR_FILE" > "$tmpfile" && \
                cat "$tmpfile" >&2 || RC=$?
            fi
            if [ $RC -ne 0 ]; then
                wsrep_log_error "Error unpacking tar file with binlog files"
                rm "$tmpfile"
                exit 32
            fi
            # Rebuild binlog index:
            [ $binlog_cd -ne 0 ] && cd "$DATA_DIR"
            while read bin_file || [ -n "$bin_file" ]; do
                echo "$binlog_dir${binlog_dir:+/}$bin_file" >> "$binlog_index"
            done < "$tmpfile"
            rm "$tmpfile"
            cd "$OLD_PWD"
        fi
    fi

    simulate_long_sst

    # Remove special tags from the magic file, and from the output:
    coords=$(head -n1 "$MAGIC_FILE")
    wsrep_log_info "Galera co-ords from recovery: $coords"
    echo "$coords" # Output : UUID:seqno wsrep_gtid_domain_id
fi

wsrep_log_info "$WSREP_METHOD $WSREP_TRANSFER_TYPE completed on $WSREP_SST_OPT_ROLE"
exit 0
