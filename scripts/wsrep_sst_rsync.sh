#!/bin/sh -ue

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
# MA  02110-1301  USA.

# This is a reference script for rsync-based state snapshot tansfer

RSYNC_PID=                                      # rsync pid file
RSYNC_CONF=                                     # rsync configuration file
RSYNC_REAL_PID=                                 # rsync process id

OS=$(uname)
[ "$OS" = "Darwin" ] && export -n LD_LIBRARY_PATH

# Setting the path for lsof on CentOS
export PATH="/usr/sbin:/sbin:$PATH"

. $(dirname $0)/wsrep_sst_common

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
    local pid_file=$1
    [ -r "$pid_file" ] && ps -p $(cat $pid_file) >/dev/null 2>&1
}

check_pid_and_port()
{
    local pid_file=$1
    local rsync_pid=$2
    local rsync_addr=$3
    local rsync_port=$4

    case $OS in
    FreeBSD)
        local port_info="$(sockstat -46lp ${rsync_port} 2>/dev/null | \
            grep ":${rsync_port}")"
        local is_rsync="$(echo $port_info | \
            grep '[[:space:]]\+rsync[[:space:]]\+'"$rsync_pid" 2>/dev/null)"
        ;;
    *)
        if ! command -v lsof > /dev/null; then
          wsrep_log_error "lsof tool not found in PATH! Make sure you have it installed."
          exit 2 # ENOENT
        fi

        local port_info="$(lsof -i :$rsync_port -Pn 2>/dev/null | \
            grep "(LISTEN)")"
        local is_rsync="$(echo $port_info | \
            grep -w '^rsync[[:space:]]\+'"$rsync_pid" 2>/dev/null)"
        ;;
    esac

    local is_listening_all="$(echo $port_info | \
        grep "*:$rsync_port" 2>/dev/null)"
    local is_listening_addr="$(echo $port_info | \
        grep "$rsync_addr:$rsync_port" 2>/dev/null)"

    if [ ! -z "$is_listening_all" -o ! -z "$is_listening_addr" ]; then
        if [ -z "$is_rsync" ]; then
            wsrep_log_error "rsync daemon port '$rsync_port' has been taken"
            exit 16 # EBUSY
        fi
    fi
    check_pid $pid_file && \
        [ -n "$port_info" ] && [ -n "$is_rsync" ] && \
        [ $(cat $pid_file) -eq $rsync_pid ]
}

is_local_ip()
{
  local address="$1"
  local get_addr_bin
  if ! command -v ifconfig > /dev/null
  then
    get_addr_bin=ip
    get_addr_bin="$get_addr_bin address show"
    # Add an slash at the end, so we don't get false positive : 172.18.0.4 matches 172.18.0.41
    # ip output format is "X.X.X.X/mask"
    address="${address}/"
  else
    # Add an space at the end, so we don't get false positive : 172.18.0.4 matches 172.18.0.41
    # ifconfig output format is "X.X.X.X "
    get_addr_bin=ifconfig
    address="$address "
  fi

  $get_addr_bin | grep "$address" > /dev/null
}

MAGIC_FILE="$WSREP_SST_OPT_DATA/rsync_sst_complete"
rm -rf "$MAGIC_FILE"

BINLOG_TAR_FILE="$WSREP_SST_OPT_DATA/wsrep_sst_binlog.tar"
BINLOG_N_FILES=1
rm -f "$BINLOG_TAR_FILE" || :

if ! [ -z $WSREP_SST_OPT_BINLOG ]
then
    BINLOG_DIRNAME=$(dirname $WSREP_SST_OPT_BINLOG)
    BINLOG_FILENAME=$(basename $WSREP_SST_OPT_BINLOG)
fi

WSREP_LOG_DIR=${WSREP_LOG_DIR:-""}
# if WSREP_LOG_DIR env. variable is not set, try to get it from my.cnf
if [ -z "$WSREP_LOG_DIR" ]; then
    WSREP_LOG_DIR=$(parse_cnf --mysqld innodb-log-group-home-dir '')
fi

if [ -n "$WSREP_LOG_DIR" ]; then
    # handle both relative and absolute paths
    WSREP_LOG_DIR=$(cd $WSREP_SST_OPT_DATA; mkdir -p "$WSREP_LOG_DIR"; cd $WSREP_LOG_DIR; pwd -P)
else
    # default to datadir
    WSREP_LOG_DIR=$(cd $WSREP_SST_OPT_DATA; pwd -P)
fi

# Old filter - include everything except selected
# FILTER=(--exclude '*.err' --exclude '*.pid' --exclude '*.sock' \
#         --exclude '*.conf' --exclude core --exclude 'galera.*' \
#         --exclude grastate.txt --exclude '*.pem' \
#         --exclude '*.[0-9][0-9][0-9][0-9][0-9][0-9]' --exclude '*.index')

# New filter - exclude everything except dirs (schemas) and innodb files
FILTER="-f '- /lost+found' -f '- /.fseventsd' -f '- /.Trashes'
        -f '+ /wsrep_sst_binlog.tar' -f '+ /ib_lru_dump' -f '+ /ibdata*' -f '+ /*/' -f '- /*'"

if [ "$WSREP_SST_OPT_ROLE" = "donor" ]
then

    if [ $WSREP_SST_OPT_BYPASS -eq 0 ]
    then

        FLUSHED="$WSREP_SST_OPT_DATA/tables_flushed"
        ERROR="$WSREP_SST_OPT_DATA/sst_error"

        rm -rf "$FLUSHED"
        rm -rf "$ERROR"

        # Use deltaxfer only for WAN
        inv=$(basename $0)
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

        STATE="$(cat $FLUSHED)"
        rm -rf "$FLUSHED"

        sync

        if ! [ -z $WSREP_SST_OPT_BINLOG ]
        then
            # Prepare binlog files
            OLD_PWD="$(pwd)"
            cd $BINLOG_DIRNAME

            binlog_files_full=$(tail -n $BINLOG_N_FILES ${BINLOG_FILENAME}.index)
            binlog_files=""
            for ii in $binlog_files_full
            do
                binlog_files="$binlog_files $(basename $ii)"
            done
            if ! [ -z "$binlog_files" ]
            then
                wsrep_log_info "Preparing binlog files for transfer:"
                tar -cvf $BINLOG_TAR_FILE $binlog_files >&2
            fi
            cd "$OLD_PWD"
        fi

        # first, the normal directories, so that we can detect incompatible protocol
        RC=0
        eval rsync --owner --group --perms --links --specials \
              --ignore-times --inplace --dirs --delete --quiet \
              $WHOLE_FILE_OPT ${FILTER} "$WSREP_SST_OPT_DATA/" \
              rsync://$WSREP_SST_OPT_ADDR >&2 || RC=$?

        if [ "$RC" -ne 0 ]; then
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

        # second, we transfer InnoDB log files
        rsync --owner --group --perms --links --specials \
              --ignore-times --inplace --dirs --delete --quiet \
              $WHOLE_FILE_OPT -f '+ /ib_logfile[0-9]*' -f '- **' "$WSREP_LOG_DIR/" \
              rsync://$WSREP_SST_OPT_ADDR-log_dir >&2 || RC=$?

        if [ $RC -ne 0 ]; then
            wsrep_log_error "rsync innodb_log_group_home_dir returned code $RC:"
            exit 255 # unknown error
        fi

        # then, we parallelize the transfer of database directories, use . so that pathconcatenation works
        OLD_PWD="$(pwd)"
        cd $WSREP_SST_OPT_DATA

        count=1
        [ "$OS" = "Linux" ] && count=$(grep -c processor /proc/cpuinfo)
        [ "$OS" = "Darwin" -o "$OS" = "FreeBSD" ] && count=$(sysctl -n hw.ncpu)

        find . -maxdepth 1 -mindepth 1 -type d -not -name "lost+found" \
             -print0 | xargs -I{} -0 -P $count \
             rsync --owner --group --perms --links --specials \
             --ignore-times --inplace --recursive --delete --quiet \
             $WHOLE_FILE_OPT --exclude '*/ib_logfile*' "$WSREP_SST_OPT_DATA"/{}/ \
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
    rsync --archive --quiet --checksum "$MAGIC_FILE" rsync://$WSREP_SST_OPT_ADDR

    echo "done $STATE"

elif [ "$WSREP_SST_OPT_ROLE" = "joiner" ]
then
    wsrep_check_programs lsof

    touch $SST_PROGRESS_FILE
    MYSQLD_PID=$WSREP_SST_OPT_PARENT

    MODULE="rsync_sst"

    RSYNC_PID="$WSREP_SST_OPT_DATA/$MODULE.pid"

    if check_pid $RSYNC_PID
    then
        wsrep_log_error "rsync daemon already running."
        exit 114 # EALREADY
    fi
    rm -rf "$RSYNC_PID"

    ADDR=$WSREP_SST_OPT_ADDR
    RSYNC_PORT=$(echo $ADDR | awk -F ':' '{ print $2 }')
    RSYNC_ADDR=$(echo $ADDR | awk -F ':' '{ print $1 }')
    if [ -z "$RSYNC_PORT" ]
    then
        RSYNC_PORT=4444
        ADDR="$(echo $ADDR | awk -F ':' '{ print $1 }'):$RSYNC_PORT"
    fi

    trap "exit 32" HUP PIPE
    trap "exit 3"  INT TERM ABRT
    trap cleanup_joiner EXIT

    RSYNC_CONF="$WSREP_SST_OPT_DATA/$MODULE.conf"

    if [ -n "${MYSQL_TMP_DIR:-}" ] ; then
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
[$MODULE-log_dir]
    path = $WSREP_LOG_DIR
EOF

#    rm -rf "$DATA"/ib_logfile* # we don't want old logs around

    readonly RSYNC_PORT=${WSREP_SST_OPT_PORT:-4444}
    # If the IP is local listen only in it
    if is_local_ip "$RSYNC_ADDR"
    then
      rsync --daemon --no-detach --address "$RSYNC_ADDR" --port "$RSYNC_PORT" --config "$RSYNC_CONF" &
    else
      # Not local, possibly a NAT, listen in all interface
      rsync --daemon --no-detach --port "$RSYNC_PORT" --config "$RSYNC_CONF" &
      # Overwrite address with all
      RSYNC_ADDR="*"
    fi
    RSYNC_REAL_PID=$!

    until check_pid_and_port "$RSYNC_PID" "$RSYNC_REAL_PID" "$RSYNC_ADDR" "$RSYNC_PORT"
    do
        sleep 0.2
    done

    echo "ready $WSREP_SST_OPT_HOST:$RSYNC_PORT/$MODULE"

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
        kill -- -"${MYSQLD_PID}"
        sleep 1
        exit 32
    fi

    if ! [ -z $WSREP_SST_OPT_BINLOG ]
    then

        OLD_PWD="$(pwd)"
        cd $BINLOG_DIRNAME

        if [ -f $BINLOG_TAR_FILE ]
        then
            # Clean up old binlog files first
            rm -f ${BINLOG_FILENAME}.*
            wsrep_log_info "Extracting binlog files:"
            tar -xvf $BINLOG_TAR_FILE >&2
            for ii in $(ls -1 ${BINLOG_FILENAME}.*)
            do
                echo ${BINLOG_DIRNAME}/${ii} >> ${BINLOG_FILENAME}.index
            done
        fi
        cd "$OLD_PWD"

    fi
    if [ -r "$MAGIC_FILE" ]
    then
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

rm -f $BINLOG_TAR_FILE || :

exit 0
