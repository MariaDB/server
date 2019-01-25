#!/bin/bash -ue

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
[ "$OS" == "Darwin" ] && export -n LD_LIBRARY_PATH

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
    local pid_file=$1
    [ -r "$pid_file" ] && ps -p $(cat $pid_file) >/dev/null 2>&1
}

check_pid_and_port()
{
    local pid_file=$1
    local rsync_pid=$2
    local rsync_port=$3

    if ! which lsof > /dev/null; then
      wsrep_log_error "lsof tool not found in PATH! Make sure you have it installed."
      exit 2 # ENOENT
    fi

    if ! which lsof > /dev/null; then
        wsrep_log_error "lsof tool not found in PATH! Make sure you have it installed."
        exit 2 # ENOENT
    fi

    local port_info=$(lsof -i :$rsync_port -Pn 2>/dev/null | \
        grep "(LISTEN)")
    local is_rsync=$(echo $port_info | \
        grep -wE '^(rsync|stunnel)[[:space:]]+'"$rsync_pid" 2>/dev/null)

    if [ -n "$port_info" -a -z "$is_rsync" ]; then
        wsrep_log_error "rsync daemon port '$rsync_port' has been taken"
        exit 16 # EBUSY
    fi
    check_pid $pid_file && \
        [ -n "$port_info" ] && [ -n "$is_rsync" ] && \
        [ $(cat $pid_file) -eq $rsync_pid ]
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

if ! [ -z $WSREP_SST_OPT_BINLOG ]
then
    BINLOG_DIRNAME=$(dirname $WSREP_SST_OPT_BINLOG)
    BINLOG_FILENAME=$(basename $WSREP_SST_OPT_BINLOG)
    BINLOG_INDEX_DIRNAME=$(dirname $WSREP_SST_OPT_BINLOG)
    BINLOG_INDEX_FILENAME=$(basename $WSREP_SST_OPT_BINLOG)
fi

if ! [ -z $WSREP_SST_OPT_BINLOG_INDEX ]
then
    BINLOG_INDEX_DIRNAME=$(dirname $WSREP_SST_OPT_BINLOG_INDEX)
    BINLOG_INDEX_FILENAME=$(basename $WSREP_SST_OPT_BINLOG_INDEX)
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

INNODB_DATA_HOME_DIR=${INNODB_DATA_HOME_DIR:-""}
# Try to set INNODB_DATA_HOME_DIR from the command line:
if [ ! -z "$INNODB_DATA_HOME_DIR_ARG" ]; then
    INNODB_DATA_HOME_DIR=$INNODB_DATA_HOME_DIR_ARG
fi
# if INNODB_DATA_HOME_DIR env. variable is not set, try to get it from my.cnf
if [ -z "$INNODB_DATA_HOME_DIR" ]; then
    INNODB_DATA_HOME_DIR=$(parse_cnf mysqld$WSREP_SST_OPT_SUFFIX_VALUE innodb-data-home-dir '')
fi
if [ -z "$INNODB_DATA_HOME_DIR" ]; then
    INNODB_DATA_HOME_DIR=$(parse_cnf --mysqld innodb-data-home-dir "")
fi

if [ -n "$INNODB_DATA_HOME_DIR" ]; then
    # handle both relative and absolute paths
    INNODB_DATA_HOME_DIR=$(cd $WSREP_SST_OPT_DATA; mkdir -p "$INNODB_DATA_HOME_DIR"; cd $INNODB_DATA_HOME_DIR; pwd -P)
else
    # default to datadir
    INNODB_DATA_HOME_DIR=$(cd $WSREP_SST_OPT_DATA; pwd -P)
fi

# Old filter - include everything except selected
# FILTER=(--exclude '*.err' --exclude '*.pid' --exclude '*.sock' \
#         --exclude '*.conf' --exclude core --exclude 'galera.*' \
#         --exclude grastate.txt --exclude '*.pem' \
#         --exclude '*.[0-9][0-9][0-9][0-9][0-9][0-9]' --exclude '*.index')

# New filter - exclude everything except dirs (schemas) and innodb files
FILTER=(-f '- /lost+found'
        -f '- /.fseventsd'
        -f '- /.Trashes'
        -f '+ /wsrep_sst_binlog.tar'
        -f '- $INNODB_DATA_HOME_DIR/ib_lru_dump'
        -f '- $INNODB_DATA_HOME_DIR/ibdata*'
        -f '+ /undo*'
        -f '+ /*/'
        -f '- /*')

SSTKEY=$(parse_cnf sst tkey "")
SSTCERT=$(parse_cnf sst tcert "")
STUNNEL=""
if [ -f "$SSTKEY" ] && [ -f "$SSTCERT" ] && wsrep_check_programs stunnel
then
    STUNNEL="stunnel ${STUNNEL_CONF}"
fi

if [ "$WSREP_SST_OPT_ROLE" = "donor" ]
then

cat << EOF > "$STUNNEL_CONF"
CApath = ${SSTCERT%/*}
foreground = yes
pid = $STUNNEL_PID
debug = warning
client = yes
connect = ${WSREP_SST_OPT_ADDR%/*}
TIMEOUTclose = 0
verifyPeer = yes
EOF

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

            if ! [ -z $WSREP_SST_OPT_BINLOG_INDEX ]
               binlog_files_full=$(tail -n $BINLOG_N_FILES ${BINLOG_FILENAME}.index)
            then
               cd $BINLOG_INDEX_DIRNAME
               binlog_files_full=$(tail -n $BINLOG_N_FILES ${BINLOG_INDEX_FILENAME}.index)
            fi

            cd $BINLOG_DIRNAME
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
        rsync ${STUNNEL:+--rsh="$STUNNEL"} \
              --owner --group --perms --links --specials \
              --ignore-times --inplace --dirs --delete --quiet \
              $WHOLE_FILE_OPT "${FILTER[@]}" "$WSREP_SST_OPT_DATA/" \
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
        [ "$OS" == "Linux" ] && count=$(grep -c processor /proc/cpuinfo)
        [ "$OS" == "Darwin" -o "$OS" == "FreeBSD" ] && count=$(sysctl -n hw.ncpu)

        find . -maxdepth 1 -mindepth 1 -type d -not -name "lost+found" \
             -print0 | xargs -I{} -0 -P $count \
             rsync ${STUNNEL:+--rsh="$STUNNEL"} \
             --owner --group --perms --links --specials \
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
    rsync ${STUNNEL:+--rsh="$STUNNEL"} \
          --archive --quiet --checksum "$MAGIC_FILE" rsync://$WSREP_SST_OPT_ADDR

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
    if [[ ${ADDR:0:1} == '[' ]]; then
        RSYNC_PORT=$(echo $ADDR | awk -F '\\]:' '{ print $2 }')
        RSYNC_ADDR=$(echo $ADDR | awk -F '\\]:' '{ print $1 }')"]"
    else
        RSYNC_PORT=$(echo $ADDR | awk -F ':' '{ print $2 }')
        RSYNC_ADDR=$(echo $ADDR | awk -F ':' '{ print $1 }')
    fi
    if [ -z "$RSYNC_PORT" ]
    then
        RSYNC_PORT=4444
        ADDR="$RSYNC_ADDR:$RSYNC_PORT"
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
[$MODULE-data_dir]
    path = $INNODB_DATA_HOME_DIR
EOF

#    rm -rf "$DATA"/ib_logfile* # we don't want old logs around

    # listen at all interfaces (for firewalled setups)
    readonly RSYNC_PORT=${WSREP_SST_OPT_PORT:-4444}

cat << EOF > "$STUNNEL_CONF"
key = $SSTKEY
cert = $SSTCERT
foreground = yes
pid = $STUNNEL_PID
debug = warning
client = no
[rsync]
accept = $RSYNC_PORT
exec = $(which rsync)
execargs = rsync --server --daemon --config=$RSYNC_CONF .
EOF

    if [ -z "$STUNNEL" ]
    then
        # listen at all interfaces (for firewalled setups)
        rsync --daemon --no-detach --port $RSYNC_PORT --config "$RSYNC_CONF" &
        RSYNC_REAL_PID=$!
    else
        stunnel "$STUNNEL_CONF" &
        RSYNC_REAL_PID=$!
        RSYNC_PID=$STUNNEL_PID
    fi

    until check_pid_and_port $RSYNC_PID $RSYNC_REAL_PID $RSYNC_PORT
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
                if ! [ -z $WSREP_SST_OPT_BINLOG_INDEX ]
                  echo ${BINLOG_DIRNAME}/${ii} >> ${BINLOG_FILENAME}.index
		then
                  echo ${BINLOG_DIRNAME}/${ii} >> ${BINLOG_INDEX_DIRNAME}/${BINLOG_INDEX_FILENAME}.index
                fi
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
