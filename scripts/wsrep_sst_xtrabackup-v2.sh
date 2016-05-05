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

# Documentation: http://www.percona.com/doc/percona-xtradb-cluster/manual/xtrabackup_sst.html 
# Make sure to read that before proceeding!




. $(dirname $0)/wsrep_sst_common

ealgo=""
ekey=""
ekeyfile=""
encrypt=0
nproc=1
ecode=0
ssyslog=""
ssystag=""
XTRABACKUP_PID=""
SST_PORT=""
REMOTEIP=""
tcert=""
tpem=""
tkey=""
sockopt=""
progress=""
ttime=0
totime=0
lsn=""
ecmd=""
rlimit=""
# Initially
stagemsg="${WSREP_SST_OPT_ROLE}"
cpat=""
speciald=1
ib_home_dir=""
ib_log_dir=""
ib_undo_dir=""

sfmt="tar"
strmcmd=""
tfmt=""
tcmd=""
rebuild=0
rebuildcmd=""
payload=0
pvformat="-F '%N => Rate:%r Avg:%a Elapsed:%t %e Bytes: %b %p' "
pvopts="-f  -i 10 -N $WSREP_SST_OPT_ROLE "
STATDIR=""
uextra=0
disver=""

tmpopts=""
itmpdir=""
xtmpdir=""

scomp=""
sdecomp=""

# Required for backup locks
# For backup locks it is 1 sent by joiner
# 5.6.21 PXC and later can't donate to an older joiner
sst_ver=1

if which pv &>/dev/null && pv --help | grep -q FORMAT;then 
    pvopts+=$pvformat
fi
pcmd="pv $pvopts"
declare -a RC

INNOBACKUPEX_BIN=innobackupex
DATA="${WSREP_SST_OPT_DATA}"
INFO_FILE="xtrabackup_galera_info"
IST_FILE="xtrabackup_ist"
MAGIC_FILE="${DATA}/${INFO_FILE}"

# Setting the path for ss and ip
export PATH="/usr/sbin:/sbin:$PATH"

timeit(){
    local stage=$1
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
    # $encrypt -eq 1 is for internal purposes only
    if [[ $encrypt -ge 2 || $encrypt -eq -1 ]];then 
        return 
    fi

    if [[ $encrypt -eq 0 ]];then 
        if $MY_PRINT_DEFAULTS xtrabackup | grep -q encrypt;then
            wsrep_log_error "Unexpected option combination. SST may fail. Refer to http://www.percona.com/doc/percona-xtradb-cluster/manual/xtrabackup_sst.html "
        fi
        return
    fi

    if [[ $sfmt == 'tar' ]];then
        wsrep_log_info "NOTE: Xtrabackup-based encryption - encrypt=1 - cannot be enabled with tar format"
        encrypt=-1
        return
    fi

    wsrep_log_info "Xtrabackup based encryption enabled in my.cnf - Supported only from Xtrabackup 2.1.4"

    if [[ -z $ealgo ]];then
        wsrep_log_error "FATAL: Encryption algorithm empty from my.cnf, bailing out"
        exit 3
    fi

    if [[ -z $ekey && ! -r $ekeyfile ]];then
        wsrep_log_error "FATAL: Either key or keyfile must be readable"
        exit 3
    fi

    if [[ -z $ekey ]];then
        ecmd="xbcrypt --encrypt-algo=$ealgo --encrypt-key-file=$ekeyfile"
    else
        ecmd="xbcrypt --encrypt-algo=$ealgo --encrypt-key=$ekey"
    fi

    if [[ "$WSREP_SST_OPT_ROLE" == "joiner" ]];then
        ecmd+=" -d"
    fi

    stagemsg+="-XB-Encrypted"
}

get_transfer()
{
    if [[ -z $SST_PORT ]];then 
        TSST_PORT=4444
    else 
        TSST_PORT=$SST_PORT
    fi

    if [[ $tfmt == 'nc' ]];then
        if [[ ! -x `which nc` ]];then 
            wsrep_log_error "nc(netcat) not found in path: $PATH"
            exit 2
        fi
        wsrep_log_info "Using netcat as streamer"
        if [[ "$WSREP_SST_OPT_ROLE"  == "joiner" ]];then
            if nc -h | grep -q ncat;then 
                tcmd="nc -l ${TSST_PORT}"
            else 
                tcmd="nc -dl ${TSST_PORT}"
            fi
        else
            tcmd="nc ${REMOTEIP} ${TSST_PORT}"
        fi
    else
        tfmt='socat'
        wsrep_log_info "Using socat as streamer"
        if [[ ! -x `which socat` ]];then 
            wsrep_log_error "socat not found in path: $PATH"
            exit 2
        fi

        if [[ $encrypt -eq 2 || $encrypt -eq 3 ]] && ! socat -V | grep -q "WITH_OPENSSL 1";then
            wsrep_log_error "Encryption requested, but socat is not OpenSSL enabled (encrypt=$encrypt)"
            exit 2
        fi

        if [[ $encrypt -eq 2 ]];then 
            wsrep_log_info "Using openssl based encryption with socat: with crt and pem"
            if [[ -z $tpem || -z $tcert ]];then 
                wsrep_log_error "Both PEM and CRT files required"
                exit 22
            fi
            stagemsg+="-OpenSSL-Encrypted-2"
            if [[ "$WSREP_SST_OPT_ROLE"  == "joiner" ]];then
                wsrep_log_info "Decrypting with cert=${tpem}, cafile=${tcert}"
                tcmd="socat -u openssl-listen:${TSST_PORT},reuseaddr,cert=${tpem},cafile=${tcert}${sockopt} stdio"
            else
                wsrep_log_info "Encrypting with cert=${tpem}, cafile=${tcert}"
                tcmd="socat -u stdio openssl-connect:${REMOTEIP}:${TSST_PORT},cert=${tpem},cafile=${tcert}${sockopt}"
            fi
        elif [[ $encrypt -eq 3 ]];then
            wsrep_log_info "Using openssl based encryption with socat: with key and crt"
            if [[ -z $tpem || -z $tkey ]];then
                wsrep_log_error "Both certificate and key files required"
                exit 22
            fi
            stagemsg+="-OpenSSL-Encrypted-3"
            if [[ "$WSREP_SST_OPT_ROLE"  == "joiner" ]];then
                if [[ -z $tcert ]];then
                    wsrep_log_info "Decrypting with cert=${tpem}, key=${tkey}, verify=0"
                    tcmd="socat -u openssl-listen:${TSST_PORT},reuseaddr,cert=${tpem},key=${tkey},verify=0${sockopt} stdio"
                else
                    wsrep_log_info "Decrypting with cert=${tpem}, key=${tkey}, cafile=${tcert}"
                    tcmd="socat -u openssl-listen:${TSST_PORT},reuseaddr,cert=${tpem},key=${tkey},cafile=${tcert}${sockopt} stdio"
                fi
            else
                if [[ -z $tcert ]];then
                    wsrep_log_info "Encrypting with cert=${tpem}, key=${tkey}, verify=0"
                    tcmd="socat -u stdio openssl-connect:${REMOTEIP}:${TSST_PORT},cert=${tpem},key=${tkey},verify=0${sockopt}"
                else
                    wsrep_log_info "Encrypting with cert=${tpem}, key=${tkey}, cafile=${tcert}"
                    tcmd="socat -u stdio openssl-connect:${REMOTEIP}:${TSST_PORT},cert=${tpem},key=${tkey},cafile=${tcert}${sockopt}"
                fi
            fi

        else 
            if [[ "$WSREP_SST_OPT_ROLE"  == "joiner" ]];then
                tcmd="socat -u TCP-LISTEN:${TSST_PORT},reuseaddr${sockopt} stdio"
            else
                tcmd="socat -u stdio TCP:${REMOTEIP}:${TSST_PORT}${sockopt}"
            fi
        fi
    fi

}

parse_cnf()
{
    local group=$1
    local var=$2
    # print the default settings for given group using my_print_default.
    # normalize the variable names specified in cnf file (user can use _ or - for example log-bin or log_bin)
    # then grep for needed variable
    # finally get the variable value (if variables has been specified multiple time use the last value only)
    reval=$($MY_PRINT_DEFAULTS $group | awk -F= '{if ($1 ~ /_/) { gsub(/_/,"-",$1); print $1"="$2 } else { print $0 }}' | grep -- "--$var=" | cut -d= -f2- | tail -1)
    if [[ -z $reval ]];then 
        [[ -n $3 ]] && reval=$3
    fi
    echo $reval
}

get_footprint()
{
    pushd $WSREP_SST_OPT_DATA 1>/dev/null
    payload=$(find . -regex '.*\.ibd$\|.*\.MYI$\|.*\.MYD$\|.*ibdata1$' -type f -print0 | du --files0-from=- --block-size=1 -c | awk 'END { print $1 }')
    if $MY_PRINT_DEFAULTS xtrabackup | grep -q -- "--compress";then 
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

    if [[ ! -x `which pv` ]];then 
        wsrep_log_error "pv not found in path: $PATH"
        wsrep_log_error "Disabling all progress/rate-limiting"
        pcmd=""
        rlimit=""
        progress=""
        return
    fi

    if [[ -n $progress && $progress != '1' ]];then 
        if [[ -e $progress ]];then 
            pcmd+=" 2>>$progress"
        else 
            pcmd+=" 2>$progress"
        fi
    elif [[ -z $progress && -n $rlimit  ]];then 
            # When rlimit is non-zero
            pcmd="pv -q"
    fi 

    if [[ -n $rlimit && "$WSREP_SST_OPT_ROLE"  == "donor" ]];then
        wsrep_log_info "Rate-limiting SST to $rlimit"
        pcmd+=" -L \$rlimit"
    fi
}

read_cnf()
{
    sfmt=$(parse_cnf sst streamfmt "xbstream")
    tfmt=$(parse_cnf sst transferfmt "socat")
    tcert=$(parse_cnf sst tca "")
    tpem=$(parse_cnf sst tcert "")
    tkey=$(parse_cnf sst tkey "")
    encrypt=$(parse_cnf sst encrypt 0)
    sockopt=$(parse_cnf sst sockopt "")
    progress=$(parse_cnf sst progress "")
    rebuild=$(parse_cnf sst rebuild 0)
    ttime=$(parse_cnf sst time 0)
    cpat=$(parse_cnf sst cpat '.*galera\.cache$\|.*sst_in_progress$\|.*\.sst$\|.*gvwstate\.dat$\|.*grastate\.dat$\|.*\.err$\|.*\.log$\|.*RPM_UPGRADE_MARKER$\|.*RPM_UPGRADE_HISTORY$')
    ealgo=$(parse_cnf xtrabackup encrypt "")
    ekey=$(parse_cnf xtrabackup encrypt-key "")
    ekeyfile=$(parse_cnf xtrabackup encrypt-key-file "")
    scomp=$(parse_cnf sst compressor "")
    sdecomp=$(parse_cnf sst decompressor "")


    # Refer to http://www.percona.com/doc/percona-xtradb-cluster/manual/xtrabackup_sst.html 
    if [[ -z $ealgo ]];then
        ealgo=$(parse_cnf sst encrypt-algo "")
        ekey=$(parse_cnf sst encrypt-key "")
        ekeyfile=$(parse_cnf sst encrypt-key-file "")
    fi
    rlimit=$(parse_cnf sst rlimit "")
    uextra=$(parse_cnf sst use-extra 0)
    speciald=$(parse_cnf sst sst-special-dirs 1)
    iopts=$(parse_cnf sst inno-backup-opts "")
    iapts=$(parse_cnf sst inno-apply-opts "")
    impts=$(parse_cnf sst inno-move-opts "")
    stimeout=$(parse_cnf sst sst-initial-timeout 100)
    ssyslog=$(parse_cnf sst sst-syslog 0)
    ssystag=$(parse_cnf mysqld_safe syslog-tag "${SST_SYSLOG_TAG:-}")
    ssystag+="-"

    if [[ $speciald -eq 0 ]];then 
        wsrep_log_error "sst-special-dirs equal to 0 is not supported, falling back to 1"
        speciald=1
    fi 

    if [[ $ssyslog -ne -1 ]];then 
        if $MY_PRINT_DEFAULTS mysqld_safe | tr '_' '-' | grep -q -- "--syslog";then 
            ssyslog=1
        fi
    fi
}

get_stream()
{
    if [[ $sfmt == 'xbstream' ]];then 
        wsrep_log_info "Streaming with xbstream"
        if [[ "$WSREP_SST_OPT_ROLE"  == "joiner" ]];then
            strmcmd="xbstream -x"
        else
            strmcmd="xbstream -c \${INFO_FILE}"
        fi
    else
        sfmt="tar"
        wsrep_log_info "Streaming with tar"
        if [[ "$WSREP_SST_OPT_ROLE"  == "joiner" ]];then
            strmcmd="tar xfi - "
        else
            strmcmd="tar cf - \${INFO_FILE} "
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
    elif [ "${WSREP_SST_OPT_ROLE}" = "joiner" ];then
        wsrep_log_info "Removing the sst_in_progress file"
        wsrep_cleanup_progress_file
    fi
    if [[ -n $progress && -p $progress ]];then 
        wsrep_log_info "Cleaning up fifo file $progress"
        rm $progress
    fi
    if [[ -n ${STATDIR:-} ]];then 
       [[ -d $STATDIR ]] && rm -rf $STATDIR
    fi

    # Final cleanup 
    pgid=$(ps -o pgid= $$ | grep -o '[0-9]*')

    # This means no setsid done in mysqld.
    # We don't want to kill mysqld here otherwise.
    if [[ $$ -eq $pgid ]];then

        # This means a signal was delivered to the process.
        # So, more cleanup. 
        if [[ $estatus -ge 128 ]];then 
            kill -KILL -$$ || true
        fi

    fi

    exit $estatus
}

check_pid()
{
    local pid_file="$1"
    [ -r "$pid_file" ] && ps -p $(cat "$pid_file") >/dev/null 2>&1
}

cleanup_donor()
{
    # Since this is invoked just after exit NNN
    local estatus=$?
    if [[ $estatus -ne 0 ]];then 
        wsrep_log_error "Cleanup after exit with status:$estatus"
    fi

    if [[ -n ${XTRABACKUP_PID:-} ]];then 
        if check_pid $XTRABACKUP_PID
        then
            wsrep_log_error "xtrabackup process is still running. Killing... "
            kill_xtrabackup
        fi

    fi
    rm -f ${DATA}/${IST_FILE} || true

    if [[ -n $progress && -p $progress ]];then 
        wsrep_log_info "Cleaning up fifo file $progress"
        rm -f $progress || true
    fi

    wsrep_log_info "Cleaning up temporary directories"

    if [[ -n $xtmpdir ]];then 
       [[ -d $xtmpdir ]] &&  rm -rf $xtmpdir || true
    fi

    if [[ -n $itmpdir ]];then 
       [[ -d $itmpdir ]] &&  rm -rf $itmpdir || true
    fi

    # Final cleanup 
    pgid=$(ps -o pgid= $$ | grep -o '[0-9]*')

    # This means no setsid done in mysqld.
    # We don't want to kill mysqld here otherwise.
    if [[ $$ -eq $pgid ]];then

        # This means a signal was delivered to the process.
        # So, more cleanup. 
        if [[ $estatus -ge 128 ]];then 
            kill -KILL -$$ || true
        fi

    fi

    exit $estatus

}

kill_xtrabackup()
{
    local PID=$(cat $XTRABACKUP_PID)
    [ -n "$PID" -a "0" != "$PID" ] && kill $PID && (kill $PID && kill -9 $PID) || :
    wsrep_log_info "Removing xtrabackup pid file $XTRABACKUP_PID"
    rm -f "$XTRABACKUP_PID" || true
}

setup_ports()
{
    if [[ "$WSREP_SST_OPT_ROLE"  == "donor" ]];then
        SST_PORT=$(echo $WSREP_SST_OPT_ADDR | awk -F '[:/]' '{ print $2 }')
        REMOTEIP=$(echo $WSREP_SST_OPT_ADDR | awk -F ':' '{ print $1 }')
        lsn=$(echo $WSREP_SST_OPT_ADDR | awk -F '[:/]' '{ print $4 }')
        sst_ver=$(echo $WSREP_SST_OPT_ADDR | awk -F '[:/]' '{ print $5 }')
    else
        SST_PORT=$(echo ${WSREP_SST_OPT_ADDR} | awk -F ':' '{ print $2 }')
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
    echo "ready ${ADDR}/${MODULE}//$sst_ver"
}

check_extra()
{
    local use_socket=1
    if [[ $uextra -eq 1 ]];then 
        if $MY_PRINT_DEFAULTS --mysqld | tr '_' '-' | grep -- "--thread-handling=" | grep -q 'pool-of-threads';then 
            local eport=$($MY_PRINT_DEFAULTS --mysqld | tr '_' '-' | grep -- "--extra-port=" | cut -d= -f2)
            if [[ -n $eport ]];then 
                # Xtrabackup works only locally.
                # Hence, setting host to 127.0.0.1 unconditionally. 
                wsrep_log_info "SST through extra_port $eport"
                INNOEXTRA+=" --host=127.0.0.1 --port=$eport "
                use_socket=0
            else 
                wsrep_log_error "Extra port $eport null, failing"
                exit 1
            fi
        else 
            wsrep_log_info "Thread pool not set, ignore the option use_extra"
        fi
    fi
    if [[ $use_socket -eq 1 ]] && [[ -n "${WSREP_SST_OPT_SOCKET}" ]];then
        INNOEXTRA+=" --socket=${WSREP_SST_OPT_SOCKET}"
    fi
}

recv_joiner()
{
    local dir=$1
    local msg=$2 
    local tmt=$3
    local checkf=$4
    local ltcmd

    if [[ ! -d ${dir} ]];then
        # This indicates that IST is in progress
        return
    fi

    pushd ${dir} 1>/dev/null
    set +e

    if [[ $tmt -gt 0 && -x `which timeout` ]];then 
        if timeout --help | grep -q -- '-k';then 
            ltcmd="timeout -k $(( tmt+10 )) $tmt $tcmd"
        else 
            ltcmd="timeout -s9 $tmt $tcmd"
        fi
        timeit "$msg" "$ltcmd | $strmcmd; RC=( "\${PIPESTATUS[@]}" )"
    else 
        timeit "$msg" "$tcmd | $strmcmd; RC=( "\${PIPESTATUS[@]}" )"
    fi

    set -e
    popd 1>/dev/null 

    if [[ ${RC[0]} -eq 124 ]];then 
        wsrep_log_error "Possible timeout in receving first data from donor in gtid stage"
        exit 32
    fi

    for ecode in "${RC[@]}";do 
        if [[ $ecode -ne 0 ]];then 
            wsrep_log_error "Error while getting data from donor node: " \
                            "exit codes: ${RC[@]}"
            exit 32
        fi
    done

    if [[ $checkf -eq 1 && ! -r "${MAGIC_FILE}" ]];then
        # this message should cause joiner to abort
        wsrep_log_error "xtrabackup process ended without creating '${MAGIC_FILE}'"
        wsrep_log_info "Contents of datadir" 
        wsrep_log_info "$(ls -l ${dir}/*)"
        exit 32
    fi
}


send_donor()
{
    local dir=$1
    local msg=$2 

    pushd ${dir} 1>/dev/null
    set +e
    timeit "$msg" "$strmcmd | $tcmd; RC=( "\${PIPESTATUS[@]}" )"
    set -e
    popd 1>/dev/null 


    for ecode in "${RC[@]}";do 
        if [[ $ecode -ne 0 ]];then 
            wsrep_log_error "Error while getting data from donor node: " \
                            "exit codes: ${RC[@]}"
            exit 32
        fi
    done

}

if [[ ! -x `which $INNOBACKUPEX_BIN` ]];then 
    wsrep_log_error "innobackupex not in path: $PATH"
    exit 2
fi

rm -f "${MAGIC_FILE}"

if [[ ! ${WSREP_SST_OPT_ROLE} == 'joiner' && ! ${WSREP_SST_OPT_ROLE} == 'donor' ]];then 
    wsrep_log_error "Invalid role ${WSREP_SST_OPT_ROLE}"
    exit 22
fi

read_cnf
setup_ports

if ${INNOBACKUPEX_BIN} /tmp --help 2>/dev/null | grep -q -- '--version-check'; then 
    disver="--no-version-check"
fi

if [[ ${FORCE_FTWRL:-0} -eq 1 ]];then 
    wsrep_log_info "Forcing FTWRL due to environment variable FORCE_FTWRL equal to $FORCE_FTWRL"
    iopts+=" --no-backup-locks "
fi


INNOEXTRA=""

if [[ $ssyslog -eq 1 ]];then 

    if [[ ! -x `which logger` ]];then 
        wsrep_log_error "logger not in path: $PATH. Ignoring"
    else

        wsrep_log_info "Logging all stderr of SST/Innobackupex to syslog"

        exec 2> >(logger -p daemon.err -t ${ssystag}wsrep-sst-$WSREP_SST_OPT_ROLE)

        wsrep_log_error()
        {
            logger  -p daemon.err -t ${ssystag}wsrep-sst-$WSREP_SST_OPT_ROLE "$@" 
        }

        wsrep_log_info()
        {
            logger  -p daemon.info -t ${ssystag}wsrep-sst-$WSREP_SST_OPT_ROLE "$@" 
        }

        INNOAPPLY="${INNOBACKUPEX_BIN} $disver $iapts --apply-log \$rebuildcmd \${DATA} 2>&1  | logger -p daemon.err -t ${ssystag}innobackupex-apply "
        INNOMOVE="${INNOBACKUPEX_BIN} ${WSREP_SST_OPT_CONF} $disver $impts  --move-back --force-non-empty-directories \${DATA} 2>&1 | logger -p daemon.err -t ${ssystag}innobackupex-move "
        INNOBACKUP="${INNOBACKUPEX_BIN} ${WSREP_SST_OPT_CONF} $disver $iopts \$tmpopts \$INNOEXTRA --galera-info --stream=\$sfmt \$itmpdir 2> >(logger -p daemon.err -t ${ssystag}innobackupex-backup)"
    fi

else 
    INNOAPPLY="${INNOBACKUPEX_BIN} $disver $iapts --apply-log \$rebuildcmd \${DATA} &>\${DATA}/innobackup.prepare.log"
    INNOMOVE="${INNOBACKUPEX_BIN} ${WSREP_SST_OPT_CONF} $disver $impts  --move-back --force-non-empty-directories \${DATA} &>\${DATA}/innobackup.move.log"
    INNOBACKUP="${INNOBACKUPEX_BIN} ${WSREP_SST_OPT_CONF} $disver $iopts \$tmpopts \$INNOEXTRA --galera-info --stream=\$sfmt \$itmpdir 2>\${DATA}/innobackup.backup.log"
fi

get_stream
get_transfer

if [ "$WSREP_SST_OPT_ROLE" = "donor" ]
then
    trap cleanup_donor EXIT

    if [ $WSREP_SST_OPT_BYPASS -eq 0 ]
    then
        if [[ -z $sst_ver ]];then 
            wsrep_log_error "Upgrade joiner to 5.6.21 or higher for backup locks support"
            wsrep_log_error "The joiner is not supported for this version of donor"
            exit 93
        fi

        if [[ -z $(parse_cnf mysqld tmpdir "") && -z $(parse_cnf xtrabackup tmpdir "") ]];then 
            xtmpdir=$(mktemp -d)
            tmpopts=" --tmpdir=$xtmpdir "
            wsrep_log_info "Using $xtmpdir as xtrabackup temporary directory"
        fi

        itmpdir=$(mktemp -d)
        wsrep_log_info "Using $itmpdir as innobackupex temporary directory"

        if [ "$WSREP_SST_OPT_USER" != "(null)" ]; then
           INNOEXTRA+=" --user=$WSREP_SST_OPT_USER"
        fi

        if [ -n "${WSREP_SST_OPT_PSWD:-}" ]; then
           INNOEXTRA+=" --password=$WSREP_SST_OPT_PSWD"
        else
           # Empty password, used for testing, debugging etc.
           INNOEXTRA+=" --password="
        fi

        get_keys
        if [[ $encrypt -eq 1 ]];then
            if [[ -n $ekey ]];then
                INNOEXTRA+=" --encrypt=$ealgo --encrypt-key=$ekey "
            else 
                INNOEXTRA+=" --encrypt=$ealgo --encrypt-key-file=$ekeyfile "
            fi
        fi


        check_extra

        wsrep_log_info "Streaming GTID file before SST"

        # Store donor's wsrep GTID (state ID) and wsrep_gtid_domain_id
        # (separated by a space).
        echo "${WSREP_SST_OPT_GTID} ${WSREP_SST_OPT_GTID_DOMAIN_ID}" > "${MAGIC_FILE}"

        ttcmd="$tcmd"

        if [[ $encrypt -eq 1 ]];then
            if [[ -n $scomp ]];then 
                tcmd=" $ecmd | $scomp | $tcmd "
            else 
                tcmd=" $ecmd | $tcmd "
            fi
        elif [[ -n $scomp ]];then 
            tcmd=" $scomp | $tcmd "
        fi


        send_donor $DATA "${stagemsg}-gtid"

        tcmd="$ttcmd"
        if [[ -n $progress ]];then 
            get_footprint
            tcmd="$pcmd | $tcmd"
        elif [[ -n $rlimit ]];then 
            adjust_progress
            tcmd="$pcmd | $tcmd"
        fi

        wsrep_log_info "Sleeping before data transfer for SST"
        sleep 10

        wsrep_log_info "Streaming the backup to joiner at ${REMOTEIP} ${SST_PORT:-4444}"

        if [[ -n $scomp ]];then 
            tcmd="$scomp | $tcmd"
        fi

        set +e
        timeit "${stagemsg}-SST" "$INNOBACKUP | $tcmd; RC=( "\${PIPESTATUS[@]}" )"
        set -e

        if [ ${RC[0]} -ne 0 ]; then
          wsrep_log_error "${INNOBACKUPEX_BIN} finished with error: ${RC[0]}. " \
                          "Check ${DATA}/innobackup.backup.log"
          exit 22
        elif [[ ${RC[$(( ${#RC[@]}-1 ))]} -eq 1 ]];then 
          wsrep_log_error "$tcmd finished with error: ${RC[1]}"
          exit 22
        fi

        # innobackupex implicitly writes PID to fixed location in $xtmpdir
        XTRABACKUP_PID="$xtmpdir/xtrabackup_pid"


    else # BYPASS FOR IST

        wsrep_log_info "Bypassing the SST for IST"
        echo "continue" # now server can resume updating data

        # Store donor's wsrep GTID (state ID) and wsrep_gtid_domain_id
        # (separated by a space).
        echo "${WSREP_SST_OPT_GTID} ${WSREP_SST_OPT_GTID_DOMAIN_ID}" > "${MAGIC_FILE}"
        echo "1" > "${DATA}/${IST_FILE}"
        get_keys
        if [[ $encrypt -eq 1 ]];then
            if [[ -n $scomp ]];then 
                tcmd=" $ecmd | $scomp | $tcmd "
            else
                tcmd=" $ecmd | $tcmd "
            fi
        elif [[ -n $scomp ]];then 
            tcmd=" $scomp | $tcmd "
        fi
        strmcmd+=" \${IST_FILE}"

        send_donor $DATA "${stagemsg}-IST"

    fi

    echo "done ${WSREP_SST_OPT_GTID}"
    wsrep_log_info "Total time on donor: $totime seconds"

elif [ "${WSREP_SST_OPT_ROLE}" = "joiner" ]
then
    [[ -e $SST_PROGRESS_FILE ]] && wsrep_log_info "Stale sst_in_progress file: $SST_PROGRESS_FILE"
    [[ -n $SST_PROGRESS_FILE ]] && touch $SST_PROGRESS_FILE

    ib_home_dir=$(parse_cnf mysqld innodb-data-home-dir "")
    ib_log_dir=$(parse_cnf mysqld innodb-log-group-home-dir "")
    ib_undo_dir=$(parse_cnf mysqld innodb-undo-directory "")

    stagemsg="Joiner-Recv"


    sencrypted=1
    nthreads=1

    MODULE="xtrabackup_sst"

    rm -f "${DATA}/${IST_FILE}"

    # May need xtrabackup_checkpoints later on
    rm -f ${DATA}/xtrabackup_binary ${DATA}/xtrabackup_galera_info  ${DATA}/xtrabackup_logfile

    ADDR=${WSREP_SST_OPT_ADDR}
    if [ -z "${SST_PORT}" ]
    then
        SST_PORT=4444
        ADDR="$(echo ${WSREP_SST_OPT_ADDR} | awk -F ':' '{ print $1 }'):${SST_PORT}"
    fi

    wait_for_listen ${SST_PORT} ${ADDR} ${MODULE} &

    trap sig_joiner_cleanup HUP PIPE INT TERM
    trap cleanup_joiner EXIT

    if [[ -n $progress ]];then 
        adjust_progress
        tcmd+=" | $pcmd"
    fi

    get_keys
    if [[ $encrypt -eq 1 && $sencrypted -eq 1 ]];then
        if [[ -n $sdecomp ]];then 
            strmcmd=" $sdecomp | $ecmd | $strmcmd"
        else 
            strmcmd=" $ecmd | $strmcmd"
        fi
    elif [[ -n $sdecomp ]];then 
            strmcmd=" $sdecomp | $strmcmd"
    fi

    STATDIR=$(mktemp -d)
    MAGIC_FILE="${STATDIR}/${INFO_FILE}"
    recv_joiner $STATDIR  "${stagemsg}-gtid" $stimeout 1


    if ! ps -p ${WSREP_SST_OPT_PARENT} &>/dev/null
    then
        wsrep_log_error "Parent mysqld process (PID:${WSREP_SST_OPT_PARENT}) terminated unexpectedly." 
        exit 32
    fi

    if [ ! -r "${STATDIR}/${IST_FILE}" ]
    then

        if [[ -d ${DATA}/.sst ]];then
            wsrep_log_info "WARNING: Stale temporary SST directory: ${DATA}/.sst from previous state transfer. Removing"
            rm -rf ${DATA}/.sst
        fi
        mkdir -p ${DATA}/.sst
        (recv_joiner $DATA/.sst "${stagemsg}-SST" 0 0) &
        jpid=$!
        wsrep_log_info "Proceeding with SST"


        wsrep_log_info "Cleaning the existing datadir and innodb-data/log directories"
        find $ib_home_dir $ib_log_dir $ib_undo_dir $DATA -mindepth 1  -regex $cpat  -prune  -o -exec rm -rfv {} 1>&2 \+

        tempdir=$(parse_cnf mysqld log-bin "")
        if [[ -n ${tempdir:-} ]];then
            binlog_dir=$(dirname $tempdir)
            binlog_file=$(basename $tempdir)
            if [[ -n ${binlog_dir:-} && $binlog_dir != '.' && $binlog_dir != $DATA ]];then
                pattern="$binlog_dir/$binlog_file\.[0-9]+$"
                wsrep_log_info "Cleaning the binlog directory $binlog_dir as well"
                find $binlog_dir -maxdepth 1 -type f -regex $pattern -exec rm -fv {} 1>&2 \+ || true
                rm $binlog_dir/*.index || true
            fi
        fi



        TDATA=${DATA}
        DATA="${DATA}/.sst"


        MAGIC_FILE="${DATA}/${INFO_FILE}"
        wsrep_log_info "Waiting for SST streaming to complete!"
        wait $jpid

        get_proc

        if [[ ! -s ${DATA}/xtrabackup_checkpoints ]];then 
            wsrep_log_error "xtrabackup_checkpoints missing, failed innobackupex/SST on donor"
            exit 2
        fi

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

        if test -n "$(find ${DATA} -maxdepth 1 -type f -name '*.qp' -print -quit)";then

            wsrep_log_info "Compressed qpress files found"

            if [[ ! -x `which qpress` ]];then 
                wsrep_log_error "qpress not found in path: $PATH"
                exit 22
            fi

            if [[ -n $progress ]] && pv --help | grep -q 'line-mode';then
                count=$(find ${DATA} -type f -name '*.qp' | wc -l)
                count=$(( count*2 ))
                if pv --help | grep -q FORMAT;then 
                    pvopts="-f -s $count -l -N Decompression -F '%N => Rate:%r Elapsed:%t %e Progress: [%b/$count]'"
                else 
                    pvopts="-f -s $count -l -N Decompression"
                fi
                pcmd="pv $pvopts"
                adjust_progress
                dcmd="$pcmd | xargs -n 2 qpress -T${nproc}d"
            else 
                dcmd="xargs -n 2 qpress -T${nproc}d"
            fi


            # Decompress the qpress files 
            wsrep_log_info "Decompression with $nproc threads"
            timeit "Joiner-Decompression" "find ${DATA} -type f -name '*.qp' -printf '%p\n%h\n' | $dcmd"
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


        if  [[ ! -z $WSREP_SST_OPT_BINLOG ]];then

            BINLOG_DIRNAME=$(dirname $WSREP_SST_OPT_BINLOG)
            BINLOG_FILENAME=$(basename $WSREP_SST_OPT_BINLOG)

            # To avoid comparing data directory and BINLOG_DIRNAME 
            mv $DATA/${BINLOG_FILENAME}.* $BINLOG_DIRNAME/ 2>/dev/null || true

            pushd $BINLOG_DIRNAME &>/dev/null
            for bfiles in $(ls -1 ${BINLOG_FILENAME}.[0-9]*);do
                echo ${BINLOG_DIRNAME}/${bfiles} >> ${BINLOG_FILENAME}.index
            done
            popd &> /dev/null

        fi

        wsrep_log_info "Preparing the backup at ${DATA}"
        timeit "Xtrabackup prepare stage" "$INNOAPPLY"

        if [ $? -ne 0 ];
        then
            wsrep_log_error "${INNOBACKUPEX_BIN} apply finished with errors. Check ${DATA}/innobackup.prepare.log" 
            exit 22
        fi

        MAGIC_FILE="${TDATA}/${INFO_FILE}"
        set +e
        rm $TDATA/innobackup.prepare.log $TDATA/innobackup.move.log
        set -e
        wsrep_log_info "Moving the backup to ${TDATA}"
        timeit "Xtrabackup move stage" "$INNOMOVE"
        if [[ $? -eq 0 ]];then 
            wsrep_log_info "Move successful, removing ${DATA}"
            rm -rf $DATA
            DATA=${TDATA}
        else 
            wsrep_log_error "Move failed, keeping ${DATA} for further diagnosis"
            wsrep_log_error "Check ${DATA}/innobackup.move.log for details"
            exit 22
        fi


    else 
        wsrep_log_info "${IST_FILE} received from donor: Running IST"
    fi

    if [[ ! -r ${MAGIC_FILE} ]];then 
        wsrep_log_error "SST magic file ${MAGIC_FILE} not found/readable"
        exit 2
    fi
    wsrep_log_info "Galera co-ords from recovery: $(cat ${MAGIC_FILE})"
    cat "${MAGIC_FILE}" # Output : UUID:seqno wsrep_gtid_domain_id
    wsrep_log_info "Total time on joiner: $totime seconds"
fi

exit 0
