#!/bin/bash -ue

# Copyright (C) 2011 Percona Inc
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

# This is a reference script for Percona XtraBackup-based state snapshot tansfer

TMPDIR="/tmp"

. $(dirname $0)/wsrep_sst_common

cleanup_joiner()
{
#set -x
    local PID=$(ps -aef |grep nc| grep $NC_PORT  | awk '{ print $2 }')
    wsrep_log_info "Killing nc pid $PID"
    [ -n "$PID" -a "0" != "$PID" ] && kill $PID && (kill $PID && kill -9 $PID) || :
    rm -f "$MAGIC_FILE"
#set +x
}

check_pid()
{
    local pid_file=$1
    [ -r $pid_file ] && ps -p $(cat $pid_file) >/dev/null 2>&1
}

kill_xtrabackup()
{
#set -x
    local PID=$(cat $XTRABACKUP_PID)
    [ -n "$PID" -a "0" != "$PID" ] && kill $PID && (kill $PID && kill -9 $PID) || :
    rm -f "$XTRABACKUP_PID"
#set +x
}

# waits ~10 seconds for nc to open the port and then reports ready
# (regardless of timeout)
wait_for_nc()
{
    local PORT=$1
    local ADDR=$2
    local MODULE=$3
    for i in $(seq 1 50)
    do
        netstat -nptl 2>/dev/null | grep '/nc\s*$' | awk '{ print $4 }' | \
        sed 's/.*://' | grep \^${PORT}\$ >/dev/null && break
        sleep 0.2
    done
    echo "ready ${ADDR}/${MODULE}"
}

INNOBACKUPEX_BIN=innobackupex
INNOBACKUPEX_ARGS=""
NC_BIN=nc

for TOOL_BIN in INNOBACKUPEX_BIN NC_BIN ; do
  if ! which ${!TOOL_BIN} > /dev/null 2>&1
  then 
     echo "Can't find ${!TOOL_BIN} in the path"
     exit 22 # EINVAL
  fi
done

#ROLE=$1
#ADDR=$2
readonly AUTH=(${WSREP_SST_OPT_AUTH//:/ })
readonly DATA="${WSREP_SST_OPT_DATA}"
#CONF=$5

INFO_FILE="xtrabackup_galera_info"
IST_FILE="xtrabackup_ist"

MAGIC_FILE="${DATA}/${INFO_FILE}"
rm -f "${MAGIC_FILE}"

if [ "$WSREP_SST_OPT_ROLE" = "donor" ]
then

#    UUID=$6
#    SEQNO=$7
#    BYPASS=$8

    NC_PORT=$(echo $WSREP_SST_OPT_ADDR | awk -F '[:/]' '{ print $2 }')
    REMOTEIP=$(echo $WSREP_SST_OPT_ADDR | awk -F ':' '{ print $1 }')

    if [ $WSREP_SST_OPT_BYPASS -eq 0 ]
    then

        INNOBACKUPEX_ARGS="--galera-info --tmpdir=${TMPDIR} --stream=tar
                           --defaults-file=${WSREP_SST_OPT_CONF}
                           --socket=${WSREP_SST_OPT_SOCKET}"

        if [ "${AUTH[0]}" != "(null)" ]; then
           INNOBACKUPEX_ARGS="${INNOBACKUPEX_ARGS} --user=${AUTH[0]}"
        fi

        if [ ${#AUTH[*]} -eq 2 ]; then
           INNOBACKUPEX_ARGS="${INNOBACKUPEX_ARGS} --password=${AUTH[1]}"
        fi

        set +e

        # This file and variable seems to have no effect and probably should be deleted
        XTRABACKUP_PID=$(mktemp --tmpdir wsrep_sst_xtrabackupXXXX.pid)

        ${INNOBACKUPEX_BIN} ${INNOBACKUPEX_ARGS} ${TMPDIR} \
        2> ${DATA}/innobackup.backup.log | \
        ${NC_BIN} ${REMOTEIP} ${NC_PORT}

        RC=( "${PIPESTATUS[@]}" )
        set -e

        if [ ${RC[0]} -ne 0 ]; then
          wsrep_log_error "${INNOBACKUPEX_BIN} finished with error: ${RC[0]}. " \
                          "Check ${DATA}/innobackup.backup.log"
          exit 22
        elif [  ${RC[1]} -ne 0 ]; then
          wsrep_log_error "${NC_BIN} finished with error: ${RC[1]}"
          exit 22
        fi

        if check_pid ${XTRABACKUP_PID}
        then
            wsrep_log_error "xtrabackup process is still running. Killing... "
            kill_xtrabackup
            exit 22
        fi

        rm -f ${XTRABACKUP_PID}

    else # BYPASS
        STATE="${WSREP_SST_OPT_GTID}"
        echo "continue" # now server can resume updating data
        echo "${STATE}" > "${MAGIC_FILE}"
        echo "1" > "${DATA}/${IST_FILE}"
        (cd ${DATA}; tar cf - ${INFO_FILE} ${IST_FILE}) | ${NC_BIN} ${REMOTEIP} ${NC_PORT}
        rm -f ${DATA}/${IST_FILE}
    fi

    echo "done ${WSREP_SST_OPT_GTID}"

elif [ "${WSREP_SST_OPT_ROLE}" = "joiner" ]
then
    MODULE="xtrabackup_sst"

    rm -f ${DATA}/xtrabackup_*

    ADDR=${WSREP_SST_OPT_ADDR}
    NC_PORT=$(echo ${ADDR} | awk -F ':' '{ print $2 }')
    if [ -z "${NC_PORT}" ]
    then
        NC_PORT=4444
        ADDR="$(echo ${ADDR} | awk -F ':' '{ print $1 }'):${NC_PORT}"
    fi

    wait_for_nc ${NC_PORT} ${ADDR} ${MODULE} &

#    trap "exit 32" HUP PIPE
#    trap "exit 3"  INT TERM
    trap cleanup_joiner HUP PIPE INT TERM

    set +e
    ${NC_BIN} -dl ${NC_PORT}  | tar xfi  - -C ${DATA}  1>&2
    RC=( "${PIPESTATUS[@]}" )
    set -e

    wait %% # join wait_for_nc thread

    if [ ${RC[0]} -ne 0 -o ${RC[1]} -ne 0 ];
    then
        wsrep_log_error "Error while getting st data from donor node: " \
                        "${RC[0]}, ${RC[1]}"
        exit 32
    fi

    if [ ! -r "${MAGIC_FILE}" ]
    then
        # this message should cause joiner to abort
        wsrep_log_error "xtrabackup process ended without creating '${MAGIC_FILE}'"
        exit 32
    fi

    if ! ps -p ${WSREP_SST_OPT_PARENT} >/dev/null
    then
        wsrep_log_error "Parent mysqld process (PID:${WSREP_SST_OPT_PARENT}) terminated unexpectedly." >&2
        exit 32
    fi

    if [ ! -r "${IST_FILE}" ]
    then
        rm -f ${DATA}/ib_logfile*
        ${INNOBACKUPEX_BIN} --defaults-file=${WSREP_SST_OPT_CONF} --apply-log \
        --ibbackup=xtrabackup ${DATA} 1>&2 2> ${DATA}/innobackup.prepare.log
        if [ $? -ne 0 ];
        then
            wsrep_log_error "${INNOBACKUPEX_BIN} finished with errors. Check ${DATA}/innobackup.prepare.log" >&2
            exit 22
        fi
    fi

    cat "${MAGIC_FILE}" # output UUID:seqno

else
    wsrep_log_error "Unrecognized role: ${WSREP_SST_OPT_ROLE}"
    exit 22 # EINVAL
fi

exit 0
