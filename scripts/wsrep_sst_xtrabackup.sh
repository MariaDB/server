#!/bin/bash -ue
# Copyright (C) 2013 Percona Inc
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
# Dependencies:  (depending on configuration)
# xbcrypt for encryption/decryption.
# qpress for decompression. Download from http://www.quicklz.com/qpress-11-linux-x64.tar till 
# https://blueprints.launchpad.net/percona-xtrabackup/+spec/package-qpress is fixed.
# my_print_defaults to extract values from my.cnf.
# netcat for transfer.
# xbstream for streaming.

. $(dirname $0)/wsrep_sst_common

ealgo=""
ekey=""
ekeyfile=""
encrypt=0

get_keys()
{
    # There is no metadata in the stream to indicate that it is encrypted
    # So, if the cnf file on joiner contains 'encrypt' under [xtrabackup] section then 
    # it means encryption is being used
    if ! my_print_defaults -c $WSREP_SST_OPT_CONF xtrabackup | grep -q encrypt; then
        return
    fi
    wsrep_log_info "Encryption enabled in my.cnf - not supported at the moment - Bug in Xtrabackup - lp:1190343"
    ealgo=$(my_print_defaults -c $WSREP_SST_OPT_CONF xtrabackup | grep -- '--encrypt=' | cut -d= -f2)
    ekey=$(my_print_defaults -c $WSREP_SST_OPT_CONF xtrabackup | grep -- '--encrypt-key=' | cut -d= -f2)
    ekeyfile=$(my_print_defaults -c $WSREP_SST_OPT_CONF xtrabackup | grep -- '--encrypt-key-file=' | cut -d= -f2)

    if [[ -z $ealgo ]];then
        wsrep_log_error "FATAL: Encryption algorithm empty from my.cnf, bailing out"
        exit 3
    fi

    if [[ -z $ekey && ! -r $ekeyfile ]];then
        wsrep_log_error "FATAL: Either key or keyfile must be readable"
        exit 3
    fi
    encrypt=1
}

cleanup_joiner()
{
    local PID=$(ps -aef |grep nc| grep $NC_PORT  | awk '{ print $2 }')
    wsrep_log_info "Killing nc pid $PID"
    [ -n "$PID" -a "0" != "$PID" ] && kill $PID && (kill $PID && kill -9 $PID) || :
    rm -f "$MAGIC_FILE"
    if [ "${WSREP_SST_OPT_ROLE}" = "joiner" ];then
        wsrep_log_info "Removing the sst_in_progress file"
        wsrep_cleanup_progress_file
    fi
}

check_pid()
{
    local pid_file="$1"
    [ -r "$pid_file" ] && ps -p $(cat "$pid_file") >/dev/null 2>&1
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
        TMPDIR="/tmp"

        INNOBACKUPEX_ARGS="--galera-info --stream=xbstream
                           --defaults-file=${WSREP_SST_OPT_CONF}
                           --socket=${WSREP_SST_OPT_SOCKET}"

        if [ "${AUTH[0]}" != "(null)" ]; then
           INNOBACKUPEX_ARGS="${INNOBACKUPEX_ARGS} --user=${AUTH[0]}"
        fi

        if [ ${#AUTH[*]} -eq 2 ]; then
           INNOBACKUPEX_ARGS="${INNOBACKUPEX_ARGS} --password=${AUTH[1]}"
       else
           # Empty password, used for testing, debugging etc.
           INNOBACKUPEX_ARGS="${INNOBACKUPEX_ARGS} --password="
       fi


        get_keys
        if [[ $encrypt -eq 1 ]];then
            if [[ -n $ekey ]];then
                INNOBACKUPEX_ARGS="${INNOBACKUPEX_ARGS} --encrypt=$ealgo --encrypt-key=$ekey"
            else 
                INNOBACKUPEX_ARGS="${INNOBACKUPEX_ARGS} --encrypt=$ealgo --encrypt-key-file=$ekeyfile"
            fi
        fi

        set +e
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

        # innobackupex implicitly writes PID to fixed location in ${TMPDIR}
        XTRABACKUP_PID="${TMPDIR}/xtrabackup_pid"

        if check_pid "${XTRABACKUP_PID}"
        then
            wsrep_log_error "xtrabackup process is still running. Killing... "
            kill_xtrabackup
            exit 22
        fi

        rm -f "${XTRABACKUP_PID}"

    else # BYPASS
        STATE="${WSREP_SST_OPT_GTID}"
        echo "continue" # now server can resume updating data
        echo "${STATE}" > "${MAGIC_FILE}"
        echo "1" > "${DATA}/${IST_FILE}"
        get_keys
        if [[ $encrypt -eq 1 ]];then
            if [[ -n $ekey ]];then
                (cd ${DATA}; xbstream -c ${INFO_FILE} ${IST_FILE} | xbcrypt --encrypt-algo=$ealgo --encrypt-key=$ekey) | ${NC_BIN} ${REMOTEIP} ${NC_PORT}
            else 
                (cd ${DATA}; xbstream -c ${INFO_FILE} ${IST_FILE} | xbcrypt --encrypt-algo=$ealgo --encrypt-key-file=$ekeyfile) | ${NC_BIN} ${REMOTEIP} ${NC_PORT}
            fi
        else 
            (cd ${DATA}; xbstream -c ${INFO_FILE} ${IST_FILE}) | ${NC_BIN} ${REMOTEIP} ${NC_PORT}
        fi
        rm -f ${DATA}/${IST_FILE}
    fi

    echo "done ${WSREP_SST_OPT_GTID}"

elif [ "${WSREP_SST_OPT_ROLE}" = "joiner" ]
then
    [[ -e $SST_PROGRESS_FILE ]] && wsrep_log_info "Stale sst_in_progress file: $SST_PROGRESS_FILE"
    touch $SST_PROGRESS_FILE

    sencrypted=1
    ecode=0
    declare -a RC

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

    trap "exit 32" HUP PIPE
    trap "exit 3"  INT TERM
    trap cleanup_joiner EXIT

    get_keys
    set +e
    if [[ $encrypt -eq 1 && $sencrypted -eq 1 ]];then
        if [[ -n $ekey ]];then
            ${NC_BIN} -dl ${NC_PORT}  |  xbcrypt -d --encrypt-algo=$ealgo --encrypt-key=$ekey | xbstream -x -C ${DATA} 
        else 
            ${NC_BIN} -dl ${NC_PORT}  |  xbcrypt -d --encrypt-algo=$ealgo --encrypt-key-file=$ekeyfile | xbstream -x -C ${DATA}
        fi
    else 
        ${NC_BIN} -dl ${NC_PORT}  | xbstream -x -C ${DATA}  
    fi
    RC=( "${PIPESTATUS[@]}" )
    set -e

    wait %% # join wait_for_nc thread

    for ecode in "${RC[@]}";do 
        if [[ $ecode -ne 0 ]];then 
            wsrep_log_error "Error while getting data from donor node: " \
                            "exit codes: ${RC[@]}"
            wsrep_log_error "Data directory ${DATA} needs to be empty for SST:" \
                            "Manual intervention required in that case"
            exit 32
        fi
    done

    if [ ! -r "${MAGIC_FILE}" ]
    then
        # this message should cause joiner to abort
        wsrep_log_error "xtrabackup process ended without creating '${MAGIC_FILE}'"
        exit 32
    fi

    if ! ps -p ${WSREP_SST_OPT_PARENT} >/dev/null
    then
        wsrep_log_error "Parent mysqld process (PID:${WSREP_SST_OPT_PARENT}) terminated unexpectedly." 
        exit 32
    fi

    if [ ! -r "${IST_FILE}" ]
    then
        rebuild=""
        wsrep_log_info "Removing existing ib_logfile files"
        rm -f ${DATA}/ib_logfile*

        # Decrypt only if not encrypted in stream.
        # NOT USED NOW.  
        # Till https://blueprints.launchpad.net/percona-xtrabackup/+spec/add-support-for-rsync-url 
        # is implemented
        #get_keys
        if [[ $encrypt -eq 1 && $sencrypted -eq 0 ]];then
            # Decrypt the files if any
            find ${DATA} -type f -name '*.xbcrypt' -printf '%p\n'  |  while read line;do 
                input=$line
                output=${input%.xbcrypt}

                if [[ -n $ekey ]];then
                    xbcrypt -d --encrypt-algo=$ealgo --encrypt-key=$ekey -i $input > $output
                else 
                    xbcrypt -d --encrypt-algo=$ealgo --encrypt-key-file=$ekeyfile -i $input > $output
                fi
            done

            if [[ $? = 0 ]];then
                find ${DATA} -type f -name '*.xbcrypt' -delete
            fi
        fi

        # Rebuild indexes for compact backups
        if grep -q 'compact = 1' ${DATA}/xtrabackup_checkpoints;then 
            wsrep_log_info "Index compaction detected"
            rebuild="--rebuild-indexes"
        fi

        if test -n "$(find ${DATA} -maxdepth 1 -name '*.qp' -print -quit)";then

            wsrep_log_info "Compressed qpress files found"

            if [[ ! -x `which qpress` ]];then 
                wsrep_log_error "qpress not found in PATH"
                exit 22
            fi

            set +e

            wsrep_log_info "Removing existing ibdata1 file"
            rm -f ${DATA}/ibdata1

            # Decompress the qpress files 
            find ${DATA} -type f -name '*.qp' -printf '%p\n%h\n' |  xargs -P $(grep -c processor /proc/cpuinfo) -n 2 qpress -d 
            extcode=$?

            set -e

            if [[ $extcode -eq 0 ]];then
                wsrep_log_info "Removing qpress files after decompression"
                find ${DATA} -type f -name '*.qp' -delete 
                if [[ $? -ne 0 ]];then 
                    wsrep_log_error "Something went wrong with deletion of qpress files. Investigate"
                fi
            else
                wsrep_log_error "Decompression failed. Exit code: $extcode"
                exit 22
            fi
        fi

        wsrep_log_info "Preparing the backup at ${DATA}"

        ${INNOBACKUPEX_BIN} --defaults-file=${WSREP_SST_OPT_CONF} --apply-log $rebuild \
        ${DATA} 1>&2 2> ${DATA}/innobackup.prepare.log
        if [ $? -ne 0 ];
        then
            wsrep_log_error "${INNOBACKUPEX_BIN} finished with errors. Check ${DATA}/innobackup.prepare.log" 
            exit 22
        fi
    else 
        wsrep_log_info "Running IST"
    fi

    cat "${MAGIC_FILE}" # output UUID:seqno

    #Cleanup not required here since EXIT trap should be called
    #wsrep_cleanup_progress_file

else
    wsrep_log_error "Unrecognized role: ${WSREP_SST_OPT_ROLE}"
    exit 22 # EINVAL
fi

exit 0
