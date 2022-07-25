#!/bin/bash -ue
# Copyright (C) 2013 Percona Inc
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

# Optional dependencies and options documented here:
# https://www.percona.com/doc/percona-xtradb-cluster/manual/xtrabackup_sst.html
# Make sure to read that before proceeding!

. $(dirname "$0")/wsrep_sst_common
wsrep_check_datadir

ealgo=""
ekey=""
ekeyfile=""
encrypt=0
nproc=1
ecode=0
XTRABACKUP_PID=""
tcert=""
tpem=""
sockopt=""
progress=""
ttime=0
totime=0
lsn="${WSREP_SST_OPT_LSN}"
incremental=0
ecmd=""
rlimit=""

sfmt="tar"
strmcmd=""
tfmt=""
tcmd=""
rebuild=0
rebuildcmd=""
payload=0
pvformat="-F '%N => Rate:%r Avg:%a Elapsed:%t %e Bytes: %b %p' "
pvopts="-f -i 10 -N $WSREP_SST_OPT_ROLE "
uextra=0

if [ -n "$(command -v pv)" ] && pv --help | grep -qw -- '-F'; then
    pvopts+=$pvformat
fi
pcmd="pv $pvopts"
declare -a RC

INNOBACKUPEX_BIN='innobackupex'
DATA="${WSREP_SST_OPT_DATA}"
INFO_FILE="xtrabackup_galera_info"
IST_FILE="xtrabackup_ist"
MAGIC_FILE="${DATA}/${INFO_FILE}"

timeit(){
    local stage="$1"
    shift
    local cmd="$@"
    local x1 x2 took extcode

    if [[ $ttime -eq 1 ]];then
        x1=$(date +%s)
        wsrep_log_info "Evaluating $cmd"
        eval "$cmd"
        extcode=$?
        x2=$(date +%s)
        took=$(( x2-x1 ))
        wsrep_log_info "NOTE: $stage took $took seconds"
        totime=$(( totime+took ))
    else
        wsrep_log_info "Evaluating $cmd"
        eval "$cmd"
        extcode=$?
    fi
    return $extcode
}

get_keys()
{
    if [[ $encrypt -eq 2 ]];then
        return
    fi

    if [[ $encrypt -eq 0 ]];then
        if [ -n "$ealgo" -o -n "$ekey" -o -n "$ekeyfile" ]; then
            wsrep_log_error "Options for encryption are specified, " \
                            "but encryption itself is disabled. SST may fail."
        fi
        return
    fi

    if [[ "$sfmt" == 'tar' ]];then
        wsrep_log_info "NOTE: Xtrabackup-based encryption - encrypt=1 - cannot be enabled with tar format"
        encrypt=0
        return
    fi

    wsrep_log_info "Xtrabackup based encryption enabled in my.cnf - Supported only from Xtrabackup 2.1.4"

    if [[ -z "$ealgo" ]];then
        wsrep_log_error "FATAL: Encryption algorithm empty from my.cnf, bailing out"
        exit 3
    fi

    if [[ -z "$ekey" && ! -r "$ekeyfile" ]];then
        wsrep_log_error "FATAL: Either key or keyfile must be readable"
        exit 3
    fi

    if [[ -z "$ekey" ]];then
        ecmd="xbcrypt --encrypt-algo=$ealgo --encrypt-key-file=$ekeyfile"
    else
        ecmd="xbcrypt --encrypt-algo=$ealgo --encrypt-key=$ekey"
    fi

    if [[ "$WSREP_SST_OPT_ROLE" == "joiner" ]];then
        ecmd+=" -d"
    fi
}

get_transfer()
{
    TSST_PORT=$WSREP_SST_OPT_PORT

    if [[ $tfmt == 'nc' ]];then
        wsrep_log_info "Using netcat as streamer"
        wsrep_check_programs nc
        tcmd="nc"
        if [ "$WSREP_SST_OPT_ROLE" = 'joiner' ]; then
            if nc -h 2>&1 | grep -q 'ncat'; then
                wsrep_log_info "Using Ncat as streamer"
                tcmd="$tcmd -l"
            elif nc -h 2>&1 | grep -qw -- '-d'; then
                wsrep_log_info "Using Debian netcat as streamer"
                tcmd="$tcmd -dl"
                if [ $WSREP_SST_OPT_HOST_IPv6 -eq 1 ]; then
                    # When host is not explicitly specified (when only the port
                    # is specified) netcat can only bind to an IPv4 address if
                    # the "-6" option is not explicitly specified:
                    tcmd="$tcmd -6"
                fi
            else
                wsrep_log_info "Using traditional netcat as streamer"
                tcmd="$tcmd -l -p"
            fi
            tcmd="$tcmd $TSST_PORT"
        else
            # Check to see if netcat supports the '-N' flag.
            # -N Shutdown the network socket after EOF on stdin
            # If it supports the '-N' flag, then we need to use the '-N'
            # flag, otherwise the transfer will stay open after the file
            # transfer and cause the command to timeout.
            # Older versions of netcat did not need this flag and will
            # return an error if the flag is used.
            if nc -h 2>&1 | grep -qw -- '-N'; then
                tcmd="$tcmd -N"
                wsrep_log_info "Using nc -N"
            fi
            # netcat doesn't understand [] around IPv6 address
            if nc -h 2>&1 | grep -q ncat; then
                wsrep_log_info "Using Ncat as streamer"
            elif nc -h 2>&1 | grep -qw -- '-d'; then
                wsrep_log_info "Using Debian netcat as streamer"
            else
                wsrep_log_info "Using traditional netcat as streamer"
                tcmd="$tcmd -q0"
            fi
            tcmd="$tcmd $WSREP_SST_OPT_HOST_UNESCAPED $TSST_PORT"
        fi
    else
        tfmt='socat'
        wsrep_log_info "Using socat as streamer"
        wsrep_check_programs socat

        if [ $encrypt -eq 2 ] && ! socat -V | grep -q -F 'OPENSSL';then
            wsrep_log_info "NOTE: socat is not openssl enabled, falling back to plain transfer"
            encrypt=0
        fi

        if [[ $encrypt -eq 2 ]];then
            wsrep_log_info "Using openssl based encryption with socat"
            if [[ -z $tpem || -z $tcert ]];then
                wsrep_log_error "Both PEM and CRT files required"
                exit 22
            fi
            if [[ "$WSREP_SST_OPT_ROLE"  == "joiner" ]];then
                wsrep_log_info "Decrypting with PEM $tpem, CA: $tcert"
                tcmd="socat -u openssl-listen:${TSST_PORT},reuseaddr,cert=$tpem,cafile=${tcert}${sockopt} stdio"
            else
                wsrep_log_info "Encrypting with PEM $tpem, CA: $tcert"
                tcmd="socat -u stdio openssl-connect:${WSREP_SST_OPT_HOST}:${TSST_PORT},cert=$tpem,cafile=${tcert}${sockopt}"
            fi
        else
            if [[ "$WSREP_SST_OPT_ROLE"  == "joiner" ]];then
                tcmd="socat -u TCP-LISTEN:${TSST_PORT},reuseaddr${sockopt} stdio"
            else
                tcmd="socat -u stdio TCP:${WSREP_SST_OPT_HOST}:${TSST_PORT}${sockopt}"
            fi
        fi
    fi
}

get_footprint()
{
    pushd "$WSREP_SST_OPT_DATA" 1>/dev/null
    payload=$(find . -regex '.*\.ibd$\|.*\.MYI$\|.*\.MYD$\|.*ibdata1$' -type f -print0 | du --files0-from=- --block-size=1 -c -s | awk 'END { print $1 }')
    if [ $(in_config 'xtrabackup' 'compress') -eq 1 ]; then
        # QuickLZ has around 50% compression ratio
        # When compression/compaction used, the progress is only an approximate.
        payload=$(( payload*1/2 ))
    fi
    popd 1>/dev/null
    pcmd+=" -s $payload"
    adjust_progress
}

adjust_progress()
{
    if [[ -n "$progress" && "$progress" != '1' ]];then
        if [[ -e "$progress" ]];then
            pcmd+=" 2>>$progress"
        else
            pcmd+=" 2>$progress"
        fi
    elif [[ -z "$progress" && -n "$rlimit" ]];then
            # When rlimit is non-zero
            pcmd="pv -q"
    fi

    if [[ -n "$rlimit" && "$WSREP_SST_OPT_ROLE" == "donor" ]];then
        wsrep_log_info "Rate-limiting SST to $rlimit"
        pcmd+=" -L \$rlimit"
    fi
}

read_cnf()
{
    sfmt=$(parse_cnf sst streamfmt "tar")
    tfmt=$(parse_cnf sst transferfmt "socat")
    tcert=$(parse_cnf sst tca "")
    tpem=$(parse_cnf sst tcert "")
    encrypt=$(parse_cnf sst encrypt 0)
    sockopt=$(parse_cnf sst sockopt "")
    progress=$(parse_cnf sst progress "")
    rebuild=$(parse_cnf sst rebuild 0)
    ttime=$(parse_cnf sst time 0)
    incremental=$(parse_cnf sst incremental 0)
    ealgo=$(parse_cnf xtrabackup encrypt "")
    ekey=$(parse_cnf xtrabackup encrypt-key "")
    ekeyfile=$(parse_cnf xtrabackup encrypt-key-file "")

    # Refer to http://www.percona.com/doc/percona-xtradb-cluster/manual/xtrabackup_sst.html
    if [[ -z $ealgo ]];then
        ealgo=$(parse_cnf sst encrypt-algo "")
        ekey=$(parse_cnf sst encrypt-key "")
        ekeyfile=$(parse_cnf sst encrypt-key-file "")
    fi
    rlimit=$(parse_cnf sst rlimit "")
    uextra=$(parse_cnf sst use_extra 0)
}

get_stream()
{
    if [[ $sfmt == 'xbstream' ]];then
        wsrep_log_info "Streaming with xbstream"
        if [[ "$WSREP_SST_OPT_ROLE"  == "joiner" ]];then
            strmcmd="xbstream -x"
        else
            strmcmd="xbstream -c \${INFO_FILE} \${IST_FILE}"
        fi
    else
        sfmt="tar"
        wsrep_log_info "Streaming with tar"
        if [[ "$WSREP_SST_OPT_ROLE"  == "joiner" ]];then
            strmcmd="tar xfi - --recursive-unlink -h"
        else
            strmcmd="tar cf - \${INFO_FILE} \${IST_FILE}"
        fi
    fi
}

get_proc()
{
    set +e
    nproc=$(grep -c processor /proc/cpuinfo)
    [[ -z $nproc || $nproc -eq 0 ]] && nproc=1
    set -e
}

sig_joiner_cleanup()
{
    wsrep_log_error "Removing $MAGIC_FILE file due to signal"
    rm -f "$MAGIC_FILE"
}

cleanup_joiner()
{
    # Since this is invoked just after exit NNN
    local estatus=$?
    if [[ $estatus -ne 0 ]];then
        wsrep_log_error "Cleanup after exit with status:$estatus"
    fi
    if [ "${WSREP_SST_OPT_ROLE}" = "joiner" ];then
        wsrep_log_info "Removing the sst_in_progress file"
        wsrep_cleanup_progress_file
    fi
    if [[ -n "$progress" && -p "$progress" ]];then
        wsrep_log_info "Cleaning up fifo file $progress"
        rm $progress
    fi
}

cleanup_donor()
{
    # Since this is invoked just after exit NNN
    local estatus=$?
    if [[ $estatus -ne 0 ]];then
        wsrep_log_error "Cleanup after exit with status:$estatus"
    fi

    if [[ -n "$XTRABACKUP_PID" ]];then
        if check_pid "$XTRABACKUP_PID" 1
        then
            wsrep_log_error "xtrabackup process is still running. Killing... "
            cleanup_pid $CHECK_PID "$XTRABACKUP_PID"
        fi
    fi

    rm -f "${DATA}/${IST_FILE}"

    if [[ -n "$progress" && -p "$progress" ]];then
        wsrep_log_info "Cleaning up fifo file $progress"
        rm $progress
    fi
}

# waits ~10 seconds for nc to open the port and then reports ready
# (regardless of timeout)
wait_for_listen()
{
    local PORT=$1
    local ADDR=$2
    local MODULE=$3
    for i in {1..50}
    do
        ss -p state listening "( sport = :$PORT )" | grep -qE 'socat|nc' && break
        sleep 0.2
    done
    if [[ $incremental -eq 1 ]];then
        echo "ready ${ADDR}/${MODULE}/$lsn"
    else
    echo "ready ${ADDR}/${MODULE}"
    fi
}

check_extra()
{
    local use_socket=1
    if [[ $uextra -eq 1 ]];then
        local thread_handling=$(parse_cnf '--mysqld' 'thread-handling')
        if [ "$thread_handling" = 'pool-of-threads' ]; then
            local eport=$(parse_cnf --mysqld extra-port)
            if [[ -n $eport ]];then
                # Xtrabackup works only locally.
                # Hence, setting host to 127.0.0.1 unconditionally.
                wsrep_log_info "SST through extra_port $eport"
                INNOEXTRA+=" --host=127.0.0.1 --port=$eport"
                use_socket=0
            else
                wsrep_log_error "Extra port $eport null, failing"
                exit 1
            fi
        else
            wsrep_log_info "Thread pool not set, ignore the option use_extra"
        fi
    fi
    if [[ $use_socket -eq 1 ]] && [[ -n "$WSREP_SST_OPT_SOCKET" ]];then
        INNOEXTRA+=" --socket=$WSREP_SST_OPT_SOCKET"
    fi
}

if [ ! -x "$(command -v innobackupex)" ]; then
    wsrep_log_error "innobackupex not in path: $PATH"
    exit 2
fi

rm -f "${MAGIC_FILE}"

if [[ ! ${WSREP_SST_OPT_ROLE} == 'joiner' && ! ${WSREP_SST_OPT_ROLE} == 'donor' ]];then
    wsrep_log_error "Invalid role ${WSREP_SST_OPT_ROLE}"
    exit 22
fi

read_cnf
get_stream
get_transfer

INNOAPPLY="${INNOBACKUPEX_BIN} ${WSREP_SST_OPT_CONF} --apply-log \$rebuildcmd \${DATA} &>\${DATA}/innobackup.prepare.log"
INNOBACKUP="${INNOBACKUPEX_BIN} ${WSREP_SST_OPT_CONF} \$INNOEXTRA --galera-info --stream=\$sfmt \${TMPDIR} 2>\${DATA}/innobackup.backup.log"

if [ "$WSREP_SST_OPT_ROLE" = "donor" ]
then
    trap cleanup_donor EXIT

    if [ $WSREP_SST_OPT_BYPASS -eq 0 ]
    then
        usrst=0
        TMPDIR="${TMPDIR:-/tmp}"

        if [ -n "$WSREP_SST_OPT_USER" ]; then
           INNOEXTRA+=" --user=$WSREP_SST_OPT_USER"
           usrst=1
        fi

        if [ -n "$WSREP_SST_OPT_PSWD" ]; then
           INNOEXTRA+=" --password=$WSREP_SST_OPT_PSWD"
        elif [ $usrst -eq 1 ]; then
           # Empty password, used for testing, debugging etc.
           INNOEXTRA+=" --password="
        fi

        get_keys
        if [[ $encrypt -eq 1 ]];then
            if [[ -n $ekey ]];then
                INNOEXTRA+=" --encrypt=$ealgo --encrypt-key=$ekey"
            else
                INNOEXTRA+=" --encrypt=$ealgo --encrypt-key-file=$ekeyfile"
            fi
        fi

        if [[ -n $lsn ]];then
                INNOEXTRA+=" --incremental --incremental-lsn=$lsn"
        fi

        check_extra

        wsrep_log_info "Streaming the backup to joiner at ${WSREP_SST_OPT_HOST} ${WSREP_SST_OPT_PORT}"

        if [[ -n $progress ]];then
            get_footprint
            tcmd="$pcmd | $tcmd"
        elif [[ -n $rlimit ]];then
            adjust_progress
            tcmd="$pcmd | $tcmd"
        fi

        set +e
        timeit "Donor-Transfer" "$INNOBACKUP | $tcmd; RC=( "\${PIPESTATUS[@]}" )"
        set -e

        if [ ${RC[0]} -ne 0 ]; then
          wsrep_log_error "${INNOBACKUPEX_BIN} finished with error: ${RC[0]}. " \
                          "Check ${DATA}/innobackup.backup.log"
          exit 22
        elif [[ ${RC[$(( ${#RC[@]}-1 ))]} -eq 1 ]];then
          wsrep_log_error "$tcmd finished with error: ${RC[1]}"
          exit 22
        fi

        # innobackupex implicitly writes PID to fixed location in ${TMPDIR}
        XTRABACKUP_PID="${TMPDIR}/xtrabackup_pid"

    else # BYPASS FOR IST

        wsrep_log_info "Bypassing the SST for IST"
        echo "continue" # now server can resume updating data

        # Store donor's wsrep GTID (state ID) and wsrep_gtid_domain_id
        # (separated by a space)
        echo "${WSREP_SST_OPT_GTID} ${WSREP_SST_OPT_GTID_DOMAIN_ID}" > "${MAGIC_FILE}"
        echo "1" > "${DATA}/${IST_FILE}"
        get_keys
        pushd ${DATA} 1>/dev/null
        set +e
        if [[ $encrypt -eq 1 ]];then
            tcmd=" $ecmd | $tcmd"
        fi
        timeit "Donor-IST-Unencrypted-transfer" "$strmcmd | $tcmd; RC=( "\${PIPESTATUS[@]}" )"
        set -e
        popd 1>/dev/null

        for ecode in "${RC[@]}";do
            if [[ $ecode -ne 0 ]];then
                wsrep_log_error "Error while streaming data to joiner node: " \
                                "exit codes: ${RC[@]}"
                exit 1
            fi
        done

    fi

    echo "done ${WSREP_SST_OPT_GTID}"
    wsrep_log_info "Total time on donor: $totime seconds"

elif [ "${WSREP_SST_OPT_ROLE}" = "joiner" ]
then
    [[ -e "$SST_PROGRESS_FILE" ]] && wsrep_log_info "Stale sst_in_progress file: $SST_PROGRESS_FILE"
    touch "$SST_PROGRESS_FILE"

    if [[ ! -e "${DATA}/ibdata1" ]];then
        incremental=0
    fi

    if [[ $incremental -eq 1 ]];then
        wsrep_log_info "Incremental SST enabled"
        lsn=$(grep to_lsn xtrabackup_checkpoints | cut -d= -f2 | tr -d ' ')
        wsrep_log_info "Recovered LSN: $lsn"
    fi

    sencrypted=1
    nthreads=1

    MODULE="xtrabackup_sst"

    # May need xtrabackup_checkpoints later on
    rm -f "${DATA}/xtrabackup_binary" "${DATA}/xtrabackup_galera_info" "${DATA}/xtrabackup_logfile"

    ADDR="${WSREP_SST_OPT_HOST}:${WSREP_SST_OPT_PORT}"

    wait_for_listen "${WSREP_SST_OPT_PORT}" "${ADDR}" "${MODULE}" &

    trap sig_joiner_cleanup HUP PIPE INT TERM
    trap cleanup_joiner EXIT

    if [[ -n $progress ]];then
        adjust_progress
        tcmd+=" | $pcmd"
    fi

    if [[ $incremental -eq 1 ]];then
        BDATA=$DATA
        DATA="$(mktemp -d)"
        MAGIC_FILE="${DATA}/${INFO_FILE}"
    fi

    get_keys
    set +e
    if [[ $encrypt -eq 1 && $sencrypted -eq 1 ]];then
        strmcmd="$ecmd | $strmcmd"
    fi

    pushd ${DATA} 1>/dev/null
    timeit "Joiner-Recv-Unencrypted" "$tcmd | $strmcmd; RC=( "\${PIPESTATUS[@]}" )"
    popd 1>/dev/null

    set -e

    if [[ $sfmt == 'xbstream' ]];then
        # Special handling till lp:1193240 is fixed"
        if [[ ${RC[$(( ${#RC[@]}-1 ))]} -eq 1 ]];then
            wsrep_log_error "Xbstream failed"
            wsrep_log_error "Data directory ${DATA} may not be empty: lp:1193240" \
                            "Manual intervention required in that case"
            exit 32
        fi
    fi

    wait %% # join for wait_for_listen thread

    for ecode in "${RC[@]}";do
        if [[ $ecode -ne 0 ]];then
            wsrep_log_error "Error while getting data from donor node: " \
                            "exit codes: ${RC[@]}"
            exit 32
        fi
    done

    if [ ! -r "${MAGIC_FILE}" ]
    then
        # this message should cause joiner to abort
        wsrep_log_error "xtrabackup process ended without creating '${MAGIC_FILE}'"
        wsrep_log_info "Contents of datadir"
        wsrep_log_info $(ls -l "${DATA}"/**/*)
        exit 32
    fi

    if ! ps -p ${WSREP_SST_OPT_PARENT} &>/dev/null
    then
        wsrep_log_error "Parent mysqld process (PID: $WSREP_SST_OPT_PARENT) terminated unexpectedly."
        exit 32
    fi

    if [ ! -r "${DATA}/${IST_FILE}" ]
    then
        wsrep_log_info "Proceeding with SST"

        wsrep_log_info "Removing existing ib_logfile files"
        if [[ $incremental -ne 1 ]];then
            rm -f "${DATA}/ib_logfile"*
        else
            rm -f "${BDATA}/ib_logfile"*
        fi

        get_proc

        # Rebuild indexes for compact backups
        if grep -q 'compact = 1' ${DATA}/xtrabackup_checkpoints;then
            wsrep_log_info "Index compaction detected"
            rebuild=1
        fi

        if [[ $rebuild -eq 1 ]];then
            nthreads=$(parse_cnf xtrabackup rebuild-threads $nproc)
            wsrep_log_info "Rebuilding during prepare with $nthreads threads"
            rebuildcmd="--rebuild-indexes --rebuild-threads=$nthreads"
        fi

        if test -n $(find "${DATA}" -maxdepth 1 -type f -name '*.qp' -print -quit); then

            wsrep_log_info "Compressed qpress files found"

            if [ -z "$(command -v qpress)" ]; then
                wsrep_log_error "qpress not found in path: $PATH"
                exit 22
            fi

            dcmd="xargs -n 2 qpress -dT$nproc"

            if [[ -n $progress ]] && pv --help | grep -qw -- '--line-mode';then
                count=$(find "${DATA}" -type f -name '*.qp' | wc -l)
                count=$(( count*2 ))
                if pv --help | grep -qw -F '-F';then
                    pvopts="-f -s $count -l -N Decompression -F '%N => Rate:%r Elapsed:%t %e Progress: [%b/$count]'"
                else
                    pvopts="-f -s $count -l -N Decompression"
                fi
                pcmd="pv $pvopts"
                adjust_progress
                dcmd="$pcmd | $dcmd"
            fi

            wsrep_log_info "Removing existing ibdata1 file"
            rm -f "${DATA}/ibdata1"

            # Decompress the qpress files
            wsrep_log_info "Decompression with $nproc threads"
            timeit "Decompression" "find '${DATA}' -type f -name '*.qp' -printf '%p\n%h\n' | $dcmd"
            extcode=$?

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

        if [[ $incremental -eq 1 ]];then
            # Added --ibbackup=xtrabackup_55 because it fails otherwise citing connection issues.
            INNOAPPLY="${INNOBACKUPEX_BIN} ${WSREP_SST_OPT_CONF} \
                --ibbackup=xtrabackup_55 --apply-log $rebuildcmd --redo-only $BDATA --incremental-dir=${DATA} &>>${BDATA}/innobackup.prepare.log"
        fi

        wsrep_log_info "Preparing the backup at ${DATA}"
        timeit "Xtrabackup prepare stage" "$INNOAPPLY"

        if [[ $incremental -eq 1 ]];then
            wsrep_log_info "Cleaning up ${DATA} after incremental SST"
            [[ -d ${DATA} ]] && rm -rf ${DATA}
            DATA=$BDATA
        fi

        if [ $? -ne 0 ];
        then
            wsrep_log_error "${INNOBACKUPEX_BIN} finished with errors. Check ${DATA}/innobackup.prepare.log"
            exit 22
        fi
    else
        wsrep_log_info "${IST_FILE} received from donor: Running IST"
    fi

    if [[ ! -r ${MAGIC_FILE} ]];then
        wsrep_log_error "SST magic file ${MAGIC_FILE} not found/readable"
        exit 2
    fi

    cat "${MAGIC_FILE}" # Output : UUID:seqno wsrep_gtid_domain_id
    wsrep_log_info "Total time on joiner: $totime seconds"
fi

exit 0
