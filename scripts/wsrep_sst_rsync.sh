#!/bin/bash -ue

# Copyright (C) 2010-2014 Codership Oy
# Copyright (C) 2017-2021 MariaDB
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

# This is a reference script for rsync-based state snapshot tansfer

RSYNC_PID=                                      # rsync pid file
RSYNC_CONF=                                     # rsync configuration file
RSYNC_REAL_PID=                                 # rsync process id

OS=$(uname)
[ "$OS" = 'Darwin' ] && export -n LD_LIBRARY_PATH

# Setting the path for lsof on CentOS
export PATH="/usr/sbin:/sbin:$PATH"

. $(dirname $0)/wsrep_sst_common
wsrep_check_datadir

wsrep_check_programs rsync

cleanup_joiner()
{
    wsrep_log_info "Joiner cleanup. rsync PID: $RSYNC_REAL_PID"
    [ "0" != "$RSYNC_REAL_PID" ]            && \
    kill $RSYNC_REAL_PID                    && \
    sleep 0.5                               && \
    kill -9 $RSYNC_REAL_PID >/dev/null 2>&1 || \
    :
    rm -rf "$RSYNC_CONF"
    rm -f "$STUNNEL_CONF"
    rm -f "$STUNNEL_PID"
    rm -rf "$MAGIC_FILE"
    rm -rf "$RSYNC_PID"
    wsrep_log_info "Joiner cleanup done."
    if [ "${WSREP_SST_OPT_ROLE}" = "joiner" ];then
        wsrep_cleanup_progress_file
    fi
}

# Check whether rsync process is still running.
check_pid()
{
    local pid_file="$1"
    [ -r "$pid_file" ] && ps -p $(cat "$pid_file") >/dev/null 2>&1
}

check_pid_and_port()
{
    local pid_file="$1"
    local rsync_pid=$2
    local rsync_addr=$3
    local rsync_port=$4

    case $OS in
    FreeBSD)
        local port_info="$(sockstat -46lp ${rsync_port} 2>/dev/null | \
            grep ":${rsync_port}")"
        local is_rsync="$(echo $port_info | \
            grep -E '[[:space:]]+(rsync|stunnel)[[:space:]]+'"$rsync_pid" 2>/dev/null)"
        ;;
    *)
        if [ ! -x "$(command -v lsof)" ]; then
          wsrep_log_error "lsof tool not found in PATH! Make sure you have it installed."
          exit 2 # ENOENT
        fi

        local port_info="$(lsof -i :$rsync_port -Pn 2>/dev/null | \
            grep "(LISTEN)")"
        local is_rsync="$(echo $port_info | \
            grep -E '^(rsync|stunnel)[[:space:]]+'"$rsync_pid" 2>/dev/null)"
        ;;
    esac

    local is_listening_all="$(echo $port_info | \
        grep "*:$rsync_port" 2>/dev/null)"
    local is_listening_addr="$(echo $port_info | \
        grep -F "$rsync_addr:$rsync_port" 2>/dev/null)"

    if [ ! -z "$is_listening_all" -o ! -z "$is_listening_addr" ]; then
        if [ -z "$is_rsync" ]; then
            wsrep_log_error "rsync daemon port '$rsync_port' has been taken"
            exit 16 # EBUSY
        fi
    fi
    check_pid "$pid_file" && \
        [ -n "$port_info" ] && [ -n "$is_rsync" ] && \
        [ $(cat "$pid_file") -eq $rsync_pid ]
}

is_local_ip()
{
  local address="$1"
  local get_addr_bin="$(command -v ifconfig)"
  if [ -z "$get_addr_bin" ]
  then
    get_addr_bin="$(command -v ip) address show"
    # Add an slash at the end, so we don't get false positive : 172.18.0.4 matches 172.18.0.41
    # ip output format is "X.X.X.X/mask"
    address="$address/"
  else
    # Add an space at the end, so we don't get false positive : 172.18.0.4 matches 172.18.0.41
    # ifconfig output format is "X.X.X.X "
    address="$address "
  fi

  $get_addr_bin | grep -F "$address" > /dev/null
}

STUNNEL_CONF="$WSREP_SST_OPT_DATA/stunnel.conf"
rm -f "$STUNNEL_CONF"

STUNNEL_PID="$WSREP_SST_OPT_DATA/stunnel.pid"
rm -f "$STUNNEL_PID"

MAGIC_FILE="$WSREP_SST_OPT_DATA/rsync_sst_complete"
rm -rf "$MAGIC_FILE"

BINLOG_TAR_FILE="$WSREP_SST_OPT_DATA/wsrep_sst_binlog.tar"
BINLOG_N_FILES=1
rm -f "$BINLOG_TAR_FILE" || :

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

WSREP_LOG_DIR="$INNODB_LOG_GROUP_HOME"

if [ -n "$WSREP_LOG_DIR" ]; then
    # handle both relative and absolute paths
    WSREP_LOG_DIR=$(cd "$WSREP_SST_OPT_DATA"; mkdir -p "$WSREP_LOG_DIR"; cd "$WSREP_LOG_DIR"; pwd -P)
else
    # default to datadir
    WSREP_LOG_DIR=$(cd "$WSREP_SST_OPT_DATA"; pwd -P)
fi

# if no command line argument and INNODB_DATA_HOME_DIR environment variable
# is not set, try to get it from my.cnf:
if [ -z "$INNODB_DATA_HOME_DIR" ]; then
    INNODB_DATA_HOME_DIR=$(parse_cnf '--mysqld' 'innodb-data-home-dir')
fi

if [ -n "$INNODB_DATA_HOME_DIR" ]; then
    # handle both relative and absolute paths
    INNODB_DATA_HOME_DIR=$(cd "$WSREP_SST_OPT_DATA"; mkdir -p "$INNODB_DATA_HOME_DIR"; cd "$INNODB_DATA_HOME_DIR"; pwd -P)
else
    # default to datadir
    INNODB_DATA_HOME_DIR=$(cd "$WSREP_SST_OPT_DATA"; pwd -P)
fi

# if no command line argument then try to get it from my.cnf:
if [ -z "$INNODB_UNDO_DIR" ]; then
    INNODB_UNDO_DIR=$(parse_cnf '--mysqld' 'innodb-undo-directory')
fi

if [ -n "$INNODB_UNDO_DIR" ]; then
    # handle both relative and absolute paths
    INNODB_UNDO_DIR=$(cd "$WSREP_SST_OPT_DATA"; mkdir -p "$INNODB_UNDO_DIR"; cd "$INNODB_UNDO_DIR"; pwd -P)
else
    # default to datadir
    INNODB_UNDO_DIR=$(cd "$WSREP_SST_OPT_DATA"; pwd -P)
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
        -f '+ /wsrep_sst_binlog.tar'
        -f '- $INNODB_DATA_HOME_DIR/ib_lru_dump'
        -f '- $INNODB_DATA_HOME_DIR/ibdata*'
        -f '+ $INNODB_UNDO_DIR/undo*'
        -f '+ /*/'
        -f '- /*'"

# old-style SSL config
SSTKEY=$(parse_cnf 'sst' 'tkey')
SSTCERT=$(parse_cnf 'sst' 'tcert')
SSTCA=$(parse_cnf 'sst' 'tca')

check_server_ssl_config()
{
    local section="$1"
    SSTKEY=$(parse_cnf "$section" 'ssl-key')
    SSTCERT=$(parse_cnf "$section" 'ssl-cert')
    SSTCA=$(parse_cnf "$section" 'ssl-ca')
}

SSLMODE=$(parse_cnf 'sst' 'ssl-mode' | tr [:lower:] [:upper:])

if [ -z "$SSTKEY" -a -z "$SSTCERT" ]
then
    # no old-style SSL config in [sst], check for new one
    check_server_ssl_config 'sst'
    if [ -z "$SSTKEY" -a -z "$SSTCERT" ]; then
        check_server_ssl_config '--mysqld'
    fi
fi

if [ -z "$SSLMODE" ]; then
    # Implicit verification if CA is set and the SSL mode
    # is not specified by user:
    if [ -n "$SSTCA" ]; then
        if [ -x "$(command -v stunnel)" ]; then
            SSLMODE='VERIFY_CA'
        fi
    # Require SSL by default if SSL key and cert are present:
    elif [ -n "$SSTKEY" -a -n "$SSTCERT" ]; then
        SSLMODE='REQUIRED'
    fi
fi

if [ -n "$SSTCA" ]
then
    CAFILE_OPT="CAfile = $SSTCA"
else
    CAFILE_OPT=""
fi

if [ "${SSLMODE#VERIFY}" != "$SSLMODE" ]
then
    case "$SSLMODE" in
    'VERIFY_IDENTITY')
        VERIFY_OPT='verifyPeer = yes'
        ;;
    'VERIFY_CA')
        VERIFY_OPT='verifyChain = yes'
        ;;
    *)
        wsrep_log_error "Unrecognized ssl-mode option: '$SSLMODE'"
        exit 22 # EINVAL
    esac
    if [ -z "$CAFILE_OPT" ]
    then
        wsrep_log_error "Can't have ssl-mode=$SSLMODE without CA file"
        exit 22 # EINVAL
    fi
else
    VERIFY_OPT=""
fi

STUNNEL=""
if [ -n "$SSLMODE" -a "$SSLMODE" != 'DISABLED' ] && wsrep_check_programs stunnel
then
    wsrep_log_info "Using stunnel for SSL encryption: CAfile: $SSTCA, SSLMODE: $SSLMODE"
    STUNNEL="stunnel $STUNNEL_CONF"
fi

readonly SECRET_TAG="secret"

if [ "$WSREP_SST_OPT_ROLE" = 'donor' ]
then

cat << EOF > "$STUNNEL_CONF"
key = $SSTKEY
cert = $SSTCERT
${CAFILE_OPT}
foreground = yes
pid = $STUNNEL_PID
debug = warning
client = yes
connect = ${WSREP_SST_OPT_ADDR%/*}
TIMEOUTclose = 0
${VERIFY_OPT}
EOF

    if [ $WSREP_SST_OPT_BYPASS -eq 0 ]
    then

        FLUSHED="$WSREP_SST_OPT_DATA/tables_flushed"
        ERROR="$WSREP_SST_OPT_DATA/sst_error"

        rm -rf "$FLUSHED"
        rm -rf "$ERROR"

        # Use deltaxfer only for WAN
        inv=$(basename "$0")
        [ "$inv" = "wsrep_sst_rsync_wan" ] && WHOLE_FILE_OPT="" \
                                           || WHOLE_FILE_OPT="--whole-file"

        echo "flush tables"

        # Wait for :
        # (a) Tables to be flushed, AND
        # (b) Cluster state ID & wsrep_gtid_domain_id to be written to the file, OR
        # (c) ERROR file, in case flush tables operation failed.

        while [ ! -r "$FLUSHED" ] && ! grep -q ':' "$FLUSHED" >/dev/null 2>&1
        do
            # Check whether ERROR file exists.
            if [ -f "$ERROR" ]
            then
                # Flush tables operation failed.
                rm -rf "$ERROR"
                exit 255
            fi

            sleep 0.2
        done

        STATE=$(cat "$FLUSHED")
        rm -rf "$FLUSHED"

        sync

        if [ -n "$WSREP_SST_OPT_BINLOG" ]
        then
            # Prepare binlog files
            OLD_PWD="$(pwd)"
            cd "$BINLOG_DIRNAME"

            binlog_files_full=$(tail -n $BINLOG_N_FILES "${WSREP_SST_OPT_BINLOG_INDEX%.index}.index")

            binlog_files=""
            for ii in $binlog_files_full
            do
                binlog_file=$(basename "$ii")
                binlog_files="$binlog_files $binlog_file"
            done

            if [ -n "$binlog_files" ]
            then
                wsrep_log_info "Preparing binlog files for transfer:"
                tar -cvf "$BINLOG_TAR_FILE" $binlog_files >&2
            fi

            cd "$OLD_PWD"
        fi

        # first, the normal directories, so that we can detect incompatible protocol
        RC=0
        eval rsync ${STUNNEL:+--rsh=\"$STUNNEL\"} \
              --owner --group --perms --links --specials \
              --ignore-times --inplace --dirs --delete --quiet \
              $WHOLE_FILE_OPT ${FILTER} "$WSREP_SST_OPT_DATA/" \
              rsync://$WSREP_SST_OPT_ADDR >&2 || RC=$?

        if [ $RC -ne 0 ]; then
            wsrep_log_error "rsync returned code $RC:"

            case $RC in
            12) RC=71  # EPROTO
                wsrep_log_error \
                "rsync server on the other end has incompatible protocol. " \
                "Make sure you have the same version of rsync on all nodes."
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
              rsync://$WSREP_SST_OPT_ADDR-data_dir >&2 || RC=$?

        if [ $RC -ne 0 ]; then
            wsrep_log_error "rsync innodb_data_home_dir returned code $RC:"
            exit 255 # unknown error
        fi

        # second, we transfer InnoDB log files
        rsync ${STUNNEL:+--rsh="$STUNNEL"} \
              --owner --group --perms --links --specials \
              --ignore-times --inplace --dirs --delete --quiet \
              $WHOLE_FILE_OPT -f '+ /ib_logfile[0-9]*' -f '+ /aria_log.*' -f '+ /aria_log_control' -f '- **' "$WSREP_LOG_DIR/" \
              rsync://$WSREP_SST_OPT_ADDR-log_dir >&2 || RC=$?

        if [ $RC -ne 0 ]; then
            wsrep_log_error "rsync innodb_log_group_home_dir returned code $RC:"
            exit 255 # unknown error
        fi

        # then, we parallelize the transfer of database directories, use . so that pathconcatenation works
        OLD_PWD="$(pwd)"
        cd "$WSREP_SST_OPT_DATA"

        count=1
        [ "$OS" = "Linux" ] && count=$(grep -c processor /proc/cpuinfo)
        [ "$OS" = "Darwin" -o "$OS" = "FreeBSD" ] && count=$(sysctl -n hw.ncpu)

        find . -maxdepth 1 -mindepth 1 -type d -not -name "lost+found" -not -name ".zfs" \
             -print0 | xargs -I{} -0 -P $count \
             rsync ${STUNNEL:+--rsh="$STUNNEL"} \
             --owner --group --perms --links --specials \
             --ignore-times --inplace --recursive --delete --quiet \
             $WHOLE_FILE_OPT --exclude '*/ib_logfile*' --exclude "*/aria_log.*" --exclude "*/aria_log_control" "$WSREP_SST_OPT_DATA"/{}/ \
             rsync://$WSREP_SST_OPT_ADDR/{} >&2 || RC=$?

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

    echo "continue" # now server can resume updating data

    echo "$STATE" > "$MAGIC_FILE"

    if [ -n "$WSREP_SST_OPT_REMOTE_PSWD" ]; then
        # Let joiner know that we know its secret
        echo "$SECRET_TAG $WSREP_SST_OPT_REMOTE_PSWD" >> "$MAGIC_FILE"
    fi

    rsync ${STUNNEL:+--rsh="$STUNNEL"} \
          --archive --quiet --checksum "$MAGIC_FILE" rsync://$WSREP_SST_OPT_ADDR

    echo "done $STATE"

elif [ "$WSREP_SST_OPT_ROLE" = 'joiner' ]
then
    wsrep_check_programs lsof

    touch "$SST_PROGRESS_FILE"
    MYSQLD_PID="$WSREP_SST_OPT_PARENT"

    MODULE="rsync_sst"

    RSYNC_PID="$WSREP_SST_OPT_DATA/$MODULE.pid"
    # give some time for lingering rsync from previous SST to complete
    check_round=0
    while check_pid "$RSYNC_PID" && [ $check_round -lt 10 ]
    do
        wsrep_log_info "lingering rsync daemon found at startup, waiting for it to exit"
        check_round=$(( check_round + 1 ))
        sleep 1
    done

    if check_pid "$RSYNC_PID"
    then
        wsrep_log_error "rsync daemon already running."
        exit 114 # EALREADY
    fi
    rm -rf "$RSYNC_PID"

    ADDR="$WSREP_SST_OPT_ADDR"
    RSYNC_PORT="$WSREP_SST_OPT_PORT"
    RSYNC_ADDR="$WSREP_SST_OPT_HOST"

    trap "exit 32" HUP PIPE
    trap "exit 3"  INT TERM ABRT
    trap cleanup_joiner EXIT

    RSYNC_CONF="$WSREP_SST_OPT_DATA/$MODULE.conf"

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

#    rm -rf "$DATA"/ib_logfile* # we don't want old logs around

    # If the IP is local listen only in it
    if is_local_ip "$RSYNC_ADDR"
    then
      RSYNC_EXTRA_ARGS="--address $RSYNC_ADDR"
      STUNNEL_ACCEPT="$RSYNC_ADDR:$RSYNC_PORT"
    else
      # Not local, possibly a NAT, listen on all interfaces
      RSYNC_EXTRA_ARGS=""
      STUNNEL_ACCEPT="$RSYNC_PORT"
      # Overwrite address with all
      RSYNC_ADDR="*"
    fi

    if [ -z "$STUNNEL" ]
    then
      rsync --daemon --no-detach --port "$RSYNC_PORT" --config "$RSYNC_CONF" ${RSYNC_EXTRA_ARGS} &
      RSYNC_REAL_PID=$!
    else
      cat << EOF > "$STUNNEL_CONF"
key = $SSTKEY
cert = $SSTCERT
${CAFILE_OPT}
foreground = yes
pid = $STUNNEL_PID
debug = warning
client = no
[rsync]
accept = $STUNNEL_ACCEPT
exec = $(command -v rsync)
execargs = rsync --server --daemon --config='$RSYNC_CONF' .
EOF
      stunnel "$STUNNEL_CONF" &
      RSYNC_REAL_PID=$!
      RSYNC_PID="$STUNNEL_PID"
    fi

    until check_pid_and_port "$RSYNC_PID" "$RSYNC_REAL_PID" "$RSYNC_ADDR" "$RSYNC_PORT"
    do
        sleep 0.2
    done

    if [ "${SSLMODE#VERIFY}" != "$SSLMODE" ]
    then # backward-incompatible behavior
        CN=""
        if [ -n "$SSTCERT" ]
        then
            # find out my Common Name
            get_openssl
            if [ -z "$OPENSSL_BINARY" ]; then
                wsrep_log_error 'openssl not found but it is required for authentication'
                exit 42
            fi
            CN=$("$OPENSSL_BINARY" x509 -noout -subject -in "$SSTCERT" | \
                 tr "," "\n" | grep "CN =" | cut -d= -f2 | sed s/^\ // | \
                 sed s/\ %//)
        fi
        MY_SECRET=$(wsrep_gen_secret)
        # Add authentication data to address
        ADDR="$CN:$MY_SECRET@$WSREP_SST_OPT_HOST"
    else
        MY_SECRET="" # for check down in recv_joiner()
        ADDR=$WSREP_SST_OPT_HOST
    fi

    echo "ready $ADDR:$RSYNC_PORT/$MODULE"

    # wait for SST to complete by monitoring magic file
    while [ ! -r "$MAGIC_FILE" ] && check_pid "$RSYNC_PID" && \
          ps -p $MYSQLD_PID >/dev/null
    do
        sleep 1
    done

    if ! ps -p $MYSQLD_PID >/dev/null
    then
        wsrep_log_error \
        "Parent mysqld process (PID:$MYSQLD_PID) terminated unexpectedly."
        kill -- -$MYSQLD_PID
        sleep 1
        exit 32
    fi

    if [ -n "$WSREP_SST_OPT_BINLOG" ]; then

        OLD_PWD="$(pwd)"
        cd "$BINLOG_DIRNAME"

        binlog_index="${WSREP_SST_OPT_BINLOG_INDEX%.index}.index"

        # Clean up old binlog files first
        rm -f "$BINLOG_FILENAME".*
        [ -f "$binlog_index" ] && rm "$binlog_index"

        if [ -f "$BINLOG_TAR_FILE" ]; then
            wsrep_log_info "Extracting binlog files:"
            tar -xvf "$BINLOG_TAR_FILE" >> _binlog_tmp_files_$!
            while read bin_file; do
                echo "$BINLOG_DIRNAME/$bin_file" >> "$binlog_index"
            done < _binlog_tmp_files_$!
            rm -f _binlog_tmp_files_$!
        fi

        cd "$OLD_PWD"

    fi

    if [ -r "$MAGIC_FILE" ]
    then
        # check donor supplied secret
        SECRET=$(grep "$SECRET_TAG " "$MAGIC_FILE" 2>/dev/null | cut -d ' ' -f 2)
        if [ "$SECRET" != "$MY_SECRET" ]; then
            wsrep_log_error "Donor does not know my secret!"
            wsrep_log_info "Donor:'$SECRET', my:'$MY_SECRET'"
            exit 32
        fi

        # remove secret from magic file
        grep -v "$SECRET_TAG " "$MAGIC_FILE" > "$MAGIC_FILE.new"

        mv "$MAGIC_FILE.new" "$MAGIC_FILE"
        # UUID:seqno & wsrep_gtid_domain_id is received here.
        cat "$MAGIC_FILE" # Output : UUID:seqno wsrep_gtid_domain_id
    else
        # this message should cause joiner to abort
        echo "rsync process ended without creating '$MAGIC_FILE'"
    fi
    wsrep_cleanup_progress_file
#    cleanup_joiner
else
    wsrep_log_error "Unrecognized role: '$WSREP_SST_OPT_ROLE'"
    exit 22 # EINVAL
fi

rm -f "$BINLOG_TAR_FILE" || :

exit 0
