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

# Documentation:
# http://www.percona.com/doc/percona-xtradb-cluster/manual/xtrabackup_sst.html
# Make sure to read that before proceeding!

. $(dirname "$0")/wsrep_sst_common
wsrep_check_datadir

ealgo=""
ekey=""
ekeyfile=""
encrypt=0
nproc=1
ecode=0
ssyslog=""
ssystag=""
XTRABACKUP_PID=""
tca=""
tcert=""
tkey=""
sockopt=""
progress=""
ttime=0
totime=0
ecmd=""
rlimit=""
# Initially
stagemsg="${WSREP_SST_OPT_ROLE}"
cpat=""
ib_home_dir=""
ib_log_dir=""
ib_undo_dir=""

sfmt=""
strmcmd=""
tfmt=""
tcmd=""
rebuild=0
rebuildcmd=""
payload=0
pvformat="-F '%N => Rate:%r Avg:%a Elapsed:%t %e Bytes: %b %p'"
pvopts="-f -i 10 -N $WSREP_SST_OPT_ROLE"
STATDIR=""
uextra=0
disver=""

tmpopts=""
itmpdir=""
xtmpdir=""

scomp=""
sdecomp=""
ssl_dhparams=""

ssl_cert=""
ssl_ca=""
ssl_key=""

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

# Setting the path for ss and ip
export PATH="/usr/sbin:/sbin:$PATH"

OS="$(uname)"

if [ -z "$(command -v lsof)" ]; then
    wsrep_log_error "lsof tool not found in PATH! Make sure you have it installed."
    exit 2 # ENOENT
fi

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

    if [[ "$sfmt" == 'tar' ]];then
        wsrep_log_info "NOTE: Xtrabackup-based encryption - encrypt=1 - cannot be enabled with tar format"
        encrypt=-1
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
        wsrep_log_warning "Using the 'encrypt-key' option causes the encryption key"
        wsrep_log_warning "to be set via the command-line and is considered insecure."
        wsrep_log_warning "It is recommended to use the 'encrypt-key-file' option instead."

        ecmd="xbcrypt --encrypt-algo=$ealgo --encrypt-key=$ekey"
    fi

    if [[ "$WSREP_SST_OPT_ROLE" == "joiner" ]];then
        ecmd+=" -d"
    fi

    stagemsg+="-XB-Encrypted"
}

#
# verifies that the certificate matches the private key
# doing this will save us having to wait for a timeout that would
# otherwise occur.
#
# 1st param: path to the cert
# 2nd param: path to the private key
#
verify_cert_matches_key()
{
    local cert_path="$1"
    local key_path="$2"

    wsrep_check_programs openssl diff

    # generate the public key from the cert and the key
    # they should match (otherwise we can't create an SSL connection)
    if ! diff <(openssl x509 -in "$cert_path" -pubkey -noout) <(openssl rsa -in "$key_path" -pubout 2>/dev/null) >/dev/null 2>&1
    then
        wsrep_log_error "******** FATAL ERROR ************************* "
        wsrep_log_error "* The certifcate and private key do not match. "
        wsrep_log_error "* Please check your certificate and key files. "
        wsrep_log_error "********************************************** "
        exit 22
    fi
}

# Checks to see if the file exists
# If the file does not exist (or cannot be read), issues an error
# and exits
#
# 1st param: file name to be checked (for read access)
# 2nd param: 1st error message (header)
# 3rd param: 2nd error message (footer, optional)
#
verify_file_exists()
{
    local file_path="$1"
    local error_message1="$2"
    local error_message2="$3"

    if ! [[ -r "$file_path" ]]; then
        wsrep_log_error "******** FATAL ERROR ************************* "
        wsrep_log_error "* $error_message1 "
        wsrep_log_error "* Could not find/access : $file_path "

        if ! [[ -z "$error_message2" ]]; then
            wsrep_log_error "* $error_message2 "
        fi

        wsrep_log_error "********************************************** "
        exit 22
    fi
}

get_transfer()
{
    TSST_PORT="$WSREP_SST_OPT_PORT"

    if [[ $tfmt == 'nc' ]];then
        wsrep_log_info "Using netcat as streamer"
        wsrep_check_programs nc

        if [[ $encrypt -eq 2 || $encrypt -eq 3 || $encrypt -eq 4 ]]; then
            wsrep_log_error "******** FATAL ERROR *********************** "
            wsrep_log_error "* Using SSL encryption (encrypt= 2, 3, or 4) "
            wsrep_log_error "* is not supported when using nc(netcat).    "
            wsrep_log_error "******************************************** "
            exit 22
        fi

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

        donor_extra=""
        joiner_extra=""
        if [[ $encrypt -eq 2 || $encrypt -eq 3 || $encrypt -eq 4 ]]; then
            if ! socat -V | grep -q WITH_OPENSSL; then
                wsrep_log_error "******** FATAL ERROR ****************** "
                wsrep_log_error "* socat is not openssl enabled.         "
                wsrep_log_error "* Unable to encrypt SST communications. "
                wsrep_log_error "*************************************** "
                exit 2
            fi

            # Determine the socat version
            SOCAT_VERSION=$(socat -V 2>&1 | grep -m1 -oe '[0-9]\.[0-9][\.0-9]*')
            if [ -z "$SOCAT_VERSION" ]; then
                wsrep_log_error "******** FATAL ERROR ******************"
                wsrep_log_error "* Cannot determine the socat version. *"
                wsrep_log_error "***************************************"
                exit 2
            fi
            if ! check_for_version "$SOCAT_VERSION" "1.7.3"; then
                # socat versions < 1.7.3 will have 512-bit dhparams (too small)
                # so create 2048-bit dhparams and send that as a parameter:
                if [[ "$WSREP_SST_OPT_ROLE"  == "joiner" ]]; then
                    # dhparams check (will create ssl_dhparams if needed)
                    check_for_dhparams
                    joiner_extra=",dhparam='$ssl_dhparams'"
                fi
            else
                # socat version >= 1.7.3, checks to see if the peername matches
                # the hostname, then set commonname="" to disable the peername
                # checks:
                donor_extra=',commonname=""'
            fi
        fi

        if [[ $encrypt -eq 2 ]]; then
            wsrep_log_warning "**** WARNING **** encrypt=2 is deprecated and will be removed in a future release"
            wsrep_log_info "Using openssl based encryption with socat: with crt and ca"

            verify_file_exists "$tcert" "Both certificate and CA files are required." \
                                        "Please check the 'tcert' option.           "
            verify_file_exists "$tca" "Both certificate and CA files are required." \
                                      "Please check the 'tca' option.             "

            stagemsg+="-OpenSSL-Encrypted-2"
            if [[ "$WSREP_SST_OPT_ROLE"  == "joiner" ]];then
                wsrep_log_info "Decrypting with CERT: $tcert, CA: $tca"
                tcmd="socat -u openssl-listen:${TSST_PORT},reuseaddr,cert='${tcert}',cafile='${tca}'${joiner_extra}${sockopt} stdio"
            else
                wsrep_log_info "Encrypting with CERT: $tcert, CA: $tca"
                tcmd="socat -u stdio openssl-connect:${WSREP_SST_OPT_HOST}:${TSST_PORT},cert='${tcert}',cafile='${tca}'${donor_extra}${sockopt}"
            fi
        elif [[ $encrypt -eq 3 ]];then
            wsrep_log_warning "**** WARNING **** encrypt=3 is deprecated and will be removed in a future release"
            wsrep_log_info "Using openssl based encryption with socat: with key and crt"

            verify_file_exists "$tcert" "Both certificate and key files are required." \
                                        "Please check the 'tcert' option.            "
            verify_file_exists "$tkey" "Both certificate and key files are required." \
                                       "Please check the 'tkey' option.             "

            stagemsg+="-OpenSSL-Encrypted-3"
            if [[ "$WSREP_SST_OPT_ROLE"  == "joiner" ]];then
                wsrep_log_info "Decrypting with CERT: $tcert, KEY: $tkey"
                tcmd="socat -u openssl-listen:${TSST_PORT},reuseaddr,cert='${tcert}',key='${tkey}',verify=0${joiner_extra}${sockopt} stdio"
            else
                wsrep_log_info "Encrypting with CERT: $tcert, KEY: $tkey"
                tcmd="socat -u stdio openssl-connect:${WSREP_SST_OPT_HOST}:${TSST_PORT},cert='${tcert}',key='${tkey}',verify=0${sockopt}"
            fi
        elif [[ $encrypt -eq 4 ]]; then
            wsrep_log_info "Using openssl based encryption with socat: with key, crt, and ca"

            verify_file_exists "$ssl_ca" "CA, certificate, and key files are required." \
                                         "Please check the 'ssl-ca' option.           "
            verify_file_exists "$ssl_cert" "CA, certificate, and key files are required." \
                                           "Please check the 'ssl-cert' option.         "
            verify_file_exists "$ssl_key" "CA, certificate, and key files are required." \
                                          "Please check the 'ssl-key' option.          "

            # Check to see that the key matches the cert
            verify_cert_matches_key $ssl_cert $ssl_key

            stagemsg+="-OpenSSL-Encrypted-4"
            if [[ "$WSREP_SST_OPT_ROLE"  == "joiner" ]]; then
                wsrep_log_info "Decrypting with CERT: $ssl_cert, KEY: $ssl_key, CA: $ssl_ca"
                tcmd="socat -u openssl-listen:${TSST_PORT},reuseaddr,cert='${ssl_cert}',key='${ssl_key}',cafile='${ssl_ca}',verify=1${joiner_extra}${sockopt} stdio"
            else
                wsrep_log_info "Encrypting with CERT: $ssl_cert, KEY: $ssl_key, CA: $ssl_ca"
                tcmd="socat -u stdio openssl-connect:${WSREP_SST_OPT_HOST}:${TSST_PORT},cert='${ssl_cert}',key='${ssl_key}',cafile='${ssl_ca}',verify=1${donor_extra}${sockopt}"
            fi
        else
            if [[ $encrypt -eq 1 ]]; then
                wsrep_log_warning "**** WARNING **** encrypt=1 is deprecated and will be removed in a future release"
            fi

            if [[ "$WSREP_SST_OPT_ROLE"  == "joiner" ]]; then
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
    if [ -z "$(command -v pv)" ]; then
        wsrep_log_error "pv not found in path: $PATH"
        wsrep_log_error "Disabling all progress/rate-limiting"
        pcmd=""
        rlimit=""
        progress=""
        return
    fi

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
    sfmt=$(parse_cnf sst streamfmt "xbstream")
    tfmt=$(parse_cnf sst transferfmt "socat")
    tca=$(parse_cnf sst tca "")
    tcert=$(parse_cnf sst tcert "")
    tkey=$(parse_cnf sst tkey "")
    encrypt=$(parse_cnf sst encrypt 0)
    sockopt=$(parse_cnf sst sockopt "")
    progress=$(parse_cnf sst progress "")
    rebuild=$(parse_cnf sst rebuild 0)
    ttime=$(parse_cnf sst time 0)
    cpat='.*\.pem$\|.*init\.ok$\|.*galera\.cache$\|.*sst_in_progress$\|.*\.sst$\|.*gvwstate\.dat$\|.*grastate\.dat$\|.*\.err$\|.*\.log$\|.*RPM_UPGRADE_MARKER$\|.*RPM_UPGRADE_HISTORY$'
    [ "$OS" = 'FreeBSD' ] && cpat=$(echo "$cpat" | sed 's/\\|/|/g')
    ealgo=$(parse_cnf xtrabackup encrypt "")
    ekey=$(parse_cnf xtrabackup encrypt-key "")
    ekeyfile=$(parse_cnf xtrabackup encrypt-key-file "")
    scomp=$(parse_cnf sst compressor "")
    sdecomp=$(parse_cnf sst decompressor "")

    # Refer to http://www.percona.com/doc/percona-xtradb-cluster/manual/xtrabackup_sst.html
    if [[ -z "$ealgo" ]];then
        ealgo=$(parse_cnf sst encrypt-algo "")
        ekey=$(parse_cnf sst encrypt-key "")
        ekeyfile=$(parse_cnf sst encrypt-key-file "")
    fi

    # Pull the parameters needed for encrypt=4
    ssl_ca=$(parse_cnf sst ssl-ca "")
    if [[ -z "$ssl_ca" ]]; then
        ssl_ca=$(parse_cnf --mysqld ssl-ca "")
    fi
    ssl_cert=$(parse_cnf sst ssl-cert "")
    if [[ -z "$ssl_cert" ]]; then
        ssl_cert=$(parse_cnf --mysqld ssl-cert "")
    fi
    ssl_key=$(parse_cnf sst ssl-key "")
    if [[ -z "$ssl_key" ]]; then
        ssl_key=$(parse_cnf --mysqld ssl-key "")
    fi

    rlimit=$(parse_cnf sst rlimit "")
    uextra=$(parse_cnf sst use-extra 0)
    iopts=$(parse_cnf sst inno-backup-opts "")
    iapts=$(parse_cnf sst inno-apply-opts "")
    impts=$(parse_cnf sst inno-move-opts "")
    stimeout=$(parse_cnf sst sst-initial-timeout 100)
    ssyslog=$(parse_cnf sst sst-syslog 0)
    ssystag=$(parse_cnf mysqld_safe syslog-tag "${SST_SYSLOG_TAG:-}")
    ssystag+="-"

    if [ $ssyslog -ne -1 ]; then
        ssyslog=$(in_config 'mysqld_safe' 'syslog')
    fi
}

get_stream()
{
    if [ "$sfmt" = 'mbstream' -o "$sfmt" = 'xbstream' ]; then
        XBSTREAM_BIN=$(command -v "$sfmt")
        if [ -z "$XBSTREAM_BIN" ]; then
            if [ -z "$XBSTREAM_BIN" ]; then
                wsrep_log_error "Streaming with $sfmt, but $sfmt not found in path"
                exit 42
            fi
        fi
        if [ "$WSREP_SST_OPT_ROLE" = 'joiner' ]; then
            strmcmd="'$XBSTREAM_BIN' -x"
        else
            strmcmd="'$XBSTREAM_BIN' -c '$INFO_FILE'"
        fi
    else
        sfmt='tar'
        if [ "$WSREP_SST_OPT_ROLE" = 'joiner' ]; then
            strmcmd='tar xfi -'
        else
            strmcmd="tar cf - '$INFO_FILE'"
        fi
    fi
    wsrep_log_info "Streaming with $sfmt"
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
    if [[ -n "$progress" && -p "$progress" ]];then
        wsrep_log_info "Cleaning up fifo file $progress"
        rm $progress
    fi
    if [[ -n "${STATDIR:-}" ]];then
       [[ -d "$STATDIR" ]] && rm -rf "$STATDIR"
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

    if [[ -n "${XTRABACKUP_PID:-}" ]];then
        if check_pid "$XTRABACKUP_PID"
        then
            wsrep_log_error "xtrabackup process is still running. Killing... "
            kill_xtrabackup
        fi
    fi

    rm -f "${DATA}/${IST_FILE}" || true

    if [[ -n "$progress" && -p "$progress" ]];then
        wsrep_log_info "Cleaning up fifo file $progress"
        rm -f "$progress" || true
    fi

    wsrep_log_info "Cleaning up temporary directories"

    if [[ -n "$xtmpdir" ]];then
       [[ -d "$xtmpdir" ]] &&  rm -rf "$xtmpdir" || true
    fi

    if [[ -n "$itmpdir" ]];then
       [[ -d "$itmpdir" ]] &&  rm -rf "$itmpdir" || true
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
    local PID=$(cat "$XTRABACKUP_PID")
    [ -n "$PID" -a "0" != "$PID" ] && kill $PID && (kill $PID && kill -9 $PID) || :
    wsrep_log_info "Removing xtrabackup pid file $XTRABACKUP_PID"
    rm -f "$XTRABACKUP_PID" || true
}

# waits ~1 minute for nc/socat to open the port and then reports ready
# (regardless of timeout)
wait_for_listen()
{
    local HOST="$1"
    local PORT="$2"
    local MODULE="$3"
    local LSOF_OUT

    for i in {1..300}
    do
        LSOF_OUT=$(lsof -sTCP:LISTEN -i TCP:${PORT} -a -c nc -c socat -F c 2> /dev/null || :)
        [ -n "${LSOF_OUT}" ] && break
        sleep 0.2
    done

    echo "ready ${HOST}:${PORT}/${MODULE}//${WSREP_SST_OPT_SST_VER:-1}"
}

check_extra()
{
    local use_socket=1
    if [ $uextra -eq 1 ]; then
        local thread_handling=$(parse_cnf '--mysqld' 'thread-handling')
        if [ "$thread_handling" = 'pool-of-threads' ]; then
            local eport=$(parse_cnf '--mysqld' 'extra-port')
            if [ -n "$eport" ]; then
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
    if [ $use_socket -eq 1 -a -n "$WSREP_SST_OPT_SOCKET" ]; then
        INNOEXTRA+=" --socket='$WSREP_SST_OPT_SOCKET'"
    fi
}

recv_joiner()
{
    local dir="$1"
    local msg="$2"
    local tmt=$3
    local checkf=$4
    local ltcmd

    if [[ ! -d "${dir}" ]];then
        # This indicates that IST is in progress
        return
    fi

    pushd "${dir}" 1>/dev/null
    set +e

    if [ $tmt -gt 0 -a -n "$(command -v timeout)" ]; then
        if timeout --help | grep -qw -- '-k';then
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
        wsrep_log_info $(ls -l "$dir/"*)
        exit 32
    fi
}

send_donor()
{
    local dir="$1"
    local msg="$2"

    pushd "${dir}" 1>/dev/null
    set +e
    timeit "$msg" "$strmcmd | $tcmd; RC=( "\${PIPESTATUS[@]}" )"
    set -e
    popd 1>/dev/null

    for ecode in "${RC[@]}";do
        if [[ $ecode -ne 0 ]];then
            wsrep_log_error "Error while sending data to joiner node: " \
                            "exit codes: ${RC[@]}"
            exit 32
        fi
    done
}

monitor_process()
{
    local sst_stream_pid=$1

    while true ; do

        if ! ps --pid "${WSREP_SST_OPT_PARENT}" &>/dev/null; then
            wsrep_log_error "Parent mysqld process (PID: $WSREP_SST_OPT_PARENT) terminated unexpectedly."
            kill -- -"${WSREP_SST_OPT_PARENT}"
            exit 32
        fi

        if ! ps --pid "${sst_stream_pid}" &>/dev/null; then
            break
        fi

        sleep 0.1

    done
}

innobackup=$(command -v "$INNOBACKUPEX_BIN")
if [ ! -x "$innobackup" ]; then
    wsrep_log_error "innobackupex not in path: $PATH"
    exit 2
fi

# check the version, we require XB-2.4 to ensure that we can pass the
# datadir via the command-line option
XB_REQUIRED_VERSION="2.3.5"

XB_VERSION=`$INNOBACKUPEX_BIN --version 2>&1 | grep -oe '[0-9]\.[0-9][\.0-9]*' | head -n1`
if [[ -z "$XB_VERSION" ]]; then
    wsrep_log_error "FATAL: Cannot determine the $INNOBACKUPEX_BIN version. Needs xtrabackup-$XB_REQUIRED_VERSION or higher to perform SST"
    exit 2
fi

if ! check_for_version "$XB_VERSION" "$XB_REQUIRED_VERSION"; then
    wsrep_log_error "FATAL: The $INNOBACKUPEX_BIN version is $XB_VERSION. Needs xtrabackup-$XB_REQUIRED_VERSION or higher to perform SST"
    exit 2
fi

rm -f "${MAGIC_FILE}"

if [[ ! "${WSREP_SST_OPT_ROLE}" == 'joiner' && ! "${WSREP_SST_OPT_ROLE}" == 'donor' ]];then
    wsrep_log_error "Invalid role ${WSREP_SST_OPT_ROLE}"
    exit 22
fi

read_cnf

if ${INNOBACKUPEX_BIN} /tmp --help 2>/dev/null | grep -qw -- '--version-check'; then
    disver='--no-version-check'
fi

iopts+=" --databases-exclude='lost+found'"

if [ ${FORCE_FTWRL:-0} -eq 1 ]; then
    wsrep_log_info "Forcing FTWRL due to environment variable FORCE_FTWRL equal to $FORCE_FTWRL"
    iopts+=' --no-backup-locks'
fi

if [[ $ssyslog -eq 1 ]];then
    if [ -z "$(command -v logger)" ]; then
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

        INNOAPPLY="2>&1 | logger -p daemon.err -t ${ssystag}innobackupex-apply"
        INNOMOVE="2>&1 | logger -p daemon.err -t ${ssystag}innobackupex-move"
        INNOBACKUP="2> >(logger -p daemon.err -t ${ssystag}innobackupex-backup)"
    fi
else
    INNOAPPLY="&>'$DATA/innobackup.prepare.log'"
    INNOMOVE="&>'$DATA/innobackup.move.log'"
    INNOBACKUP="2>'$DATA/innobackup.backup.log'"
fi

get_stream
get_transfer

# if no command line argument and INNODB_DATA_HOME_DIR environment variable
# is not set, try to get it from my.cnf:
if [ -z "$INNODB_DATA_HOME_DIR" ]; then
    INNODB_DATA_HOME_DIR=$(parse_cnf '--mysqld' 'innodb-data-home-dir')
fi

OLD_PWD="$(pwd)"

cd "$WSREP_SST_OPT_DATA"
if [ -n "$INNODB_DATA_HOME_DIR" ]; then
    # handle both relative and absolute paths
    [ ! -d "$INNODB_DATA_HOME_DIR" ] && mkdir -p "$INNODB_DATA_HOME_DIR"
    cd "$INNODB_DATA_HOME_DIR"
fi
INNODB_DATA_HOME_DIR=$(pwd -P)

cd "$OLD_PWD"

setup_commands () {
    INNOAPPLY="$INNOBACKUPEX_BIN $disver $iapts $INNOEXTRA --apply-log $rebuildcmd '$DATA' $INNOAPPLY"
    INNOMOVE="$INNOBACKUPEX_BIN $WSREP_SST_OPT_CONF $disver $impts --move-back --force-non-empty-directories '$DATA' $INNOMOVE"
    sfmt_work="$sfmt"
    if [ "$sfmt" = 'mbstream' ]; then
        sfmt_work='xbstream'
    fi
    INNOBACKUP="$INNOBACKUPEX_BIN $WSREP_SST_OPT_CONF $disver $iopts $tmpopts $INNOEXTRA --galera-info --stream=$sfmt_work '$itmpdir' $INNOBACKUP"
}

if [ "$WSREP_SST_OPT_ROLE" = "donor" ]
then
    trap cleanup_donor EXIT

    if [ $WSREP_SST_OPT_BYPASS -eq 0 ]
    then
        if [ -z "$WSREP_SST_OPT_SST_VER" ]; then
            wsrep_log_error "Upgrade joiner to 5.6.21 or higher for backup locks support"
            wsrep_log_error "The joiner is not supported for this version of donor"
            exit 93
        fi

        tmpdir=$(parse_cnf "--mysqld|sst|xtrabackup" 'tmpdir')
        if [ -z "$tmpdir" ]; then
            xtmpdir=$(mktemp -d)
            tmpopts=" --tmpdir='$xtmpdir'"
            wsrep_log_info "Using $xtmpdir as xtrabackup temporary directory"
        fi

        itmpdir=$(mktemp -d)
        wsrep_log_info "Using $itmpdir as innobackupex temporary directory"

        usrst=0
        if [ -n "$WSREP_SST_OPT_USER" ]; then
           INNOEXTRA="$INNOEXTRA --user='$WSREP_SST_OPT_USER'"
           usrst=1
        fi

        if [ -n "$WSREP_SST_OPT_PSWD" ]; then
            export MYSQL_PWD="$WSREP_SST_OPT_PSWD"
        elif [ $usrst -eq 1 ]; then
            # Empty password, used for testing, debugging etc.
            unset MYSQL_PWD
        fi

        get_keys
        check_extra

        wsrep_log_info "Streaming GTID file before SST"

        # Store donor's wsrep GTID (state ID) and wsrep_gtid_domain_id
        # (separated by a space).
        echo "${WSREP_SST_OPT_GTID} ${WSREP_SST_OPT_GTID_DOMAIN_ID}" > "${MAGIC_FILE}"

        ttcmd="$tcmd"

        if [ $encrypt -eq 1 ]; then
            if [ -n "$scomp" ]; then
                tcmd="\$ecmd | $scomp | $tcmd"
            else
                tcmd="\$ecmd | $tcmd"
            fi
        elif [ -n "$scomp" ]; then
            tcmd="$scomp | $tcmd"
        fi

        send_donor $DATA "${stagemsg}-gtid"

        # Restore the transport commmand to its original state
        tcmd="$ttcmd"
        if [ -n "$progress" ]; then
            get_footprint
            tcmd="$pcmd | $tcmd"
        elif [ -n "$rlimit" ]; then
            adjust_progress
            tcmd="$pcmd | $tcmd"
        fi

        wsrep_log_info "Sleeping before data transfer for SST"
        sleep 10

        wsrep_log_info "Streaming the backup to joiner at ${WSREP_SST_OPT_HOST}:${WSREP_SST_OPT_PORT}"

        # Add compression to the head of the stream (if specified)
        if [[ -n "$scomp" ]]; then
            tcmd="$scomp | $tcmd"
        fi

        # Add encryption to the head of the stream (if specified)
        if [[ $encrypt -eq 1 ]]; then
            tcmd="\$ecmd | $tcmd"
        fi

        setup_commands
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
        if [ $encrypt -eq 1 ]; then
            if [ -n "$scomp" ]; then
                tcmd="\$ecmd | $scomp | $tcmd"
            else
                tcmd="\$ecmd | $tcmd"
            fi
        elif [ -n "$scomp" ]; then
            tcmd="$scomp | $tcmd"
        fi
        strmcmd+=" \${IST_FILE}"

        send_donor "$DATA" "${stagemsg}-IST"

    fi

    echo "done ${WSREP_SST_OPT_GTID}"
    wsrep_log_info "Total time on donor: $totime seconds"

elif [ "${WSREP_SST_OPT_ROLE}" = "joiner" ]
then
    [[ -e "$SST_PROGRESS_FILE" ]] && wsrep_log_info "Stale sst_in_progress file: $SST_PROGRESS_FILE"
    [[ -n "$SST_PROGRESS_FILE" ]] && touch "$SST_PROGRESS_FILE"

    ib_home_dir="$INNODB_DATA_HOME_DIR"

    # if no command line argument and INNODB_LOG_GROUP_HOME is not set,
    # try to get it from my.cnf:
    if [ -z "$INNODB_LOG_GROUP_HOME" ]; then
        INNODB_LOG_GROUP_HOME=$(parse_cnf '--mysqld' 'innodb-log-group-home-dir')
    fi

    ib_log_dir="$INNODB_LOG_GROUP_HOME"

    # if no command line argument then try to get it from my.cnf:
    if [ -z "$INNODB_UNDO_DIR" ]; then
        INNODB_UNDO_DIR=$(parse_cnf '--mysqld' 'innodb-undo-directory')
    fi

    ib_undo_dir="$INNODB_UNDO_DIR"

    stagemsg="Joiner-Recv"

    sencrypted=1
    nthreads=1

    MODULE="xtrabackup_sst"

    rm -f "${DATA}/${IST_FILE}"

    # May need xtrabackup_checkpoints later on
    rm -f "${DATA}/xtrabackup_binary" "${DATA}/xtrabackup_galera_info" "${DATA}/xtrabackup_logfile"

    wait_for_listen ${WSREP_SST_OPT_HOST} ${WSREP_SST_OPT_PORT} ${MODULE} &

    trap sig_joiner_cleanup HUP PIPE INT TERM
    trap cleanup_joiner EXIT

    if [[ -n "$progress" ]];then
        adjust_progress
        tcmd+=" | $pcmd"
    fi

    get_keys
    if [ $encrypt -eq 1 -a $sencrypted -eq 1 ]; then
        if [ -n "$sdecomp" ]; then
            strmcmd="$sdecomp | \$ecmd | $strmcmd"
        else
            strmcmd="\$ecmd | $strmcmd"
        fi
    elif [ -n "$sdecomp" ]; then
            strmcmd="$sdecomp | $strmcmd"
    fi

    STATDIR=$(mktemp -d)
    MAGIC_FILE="${STATDIR}/${INFO_FILE}"
    recv_joiner "$STATDIR" "${stagemsg}-gtid" $stimeout 1

    if ! ps -p ${WSREP_SST_OPT_PARENT} &>/dev/null
    then
        wsrep_log_error "Parent mysqld process (PID: $WSREP_SST_OPT_PARENT) terminated unexpectedly."
        exit 32
    fi

    if [ ! -r "${STATDIR}/${IST_FILE}" ]
    then

        if [[ -d "${DATA}/.sst" ]];then
            wsrep_log_info "WARNING: Stale temporary SST directory: ${DATA}/.sst from previous state transfer. Removing"
            rm -rf "${DATA}/.sst"
        fi
        mkdir -p "${DATA}/.sst"
        (recv_joiner "$DATA/.sst" "${stagemsg}-SST" 0 0) &
        jpid=$!
        wsrep_log_info "Proceeding with SST"

        wsrep_log_info "Cleaning the existing datadir and innodb-data/log directories"
        if [ "${OS}" = "FreeBSD" ]; then
            find -E $ib_home_dir $ib_log_dir $ib_undo_dir $DATA -mindepth 1 -prune -regex $cpat -o -exec rm -rfv {} 1>&2 \+
        else
            find $ib_home_dir $ib_log_dir $ib_undo_dir $DATA -mindepth 1 -prune -regex $cpat -o -exec rm -rfv {} 1>&2 \+
        fi

        get_binlog

        if [ -n "$WSREP_SST_OPT_BINLOG" ]; then
            binlog_dir=$(dirname "$WSREP_SST_OPT_BINLOG")
            cd "$binlog_dir"
            wsrep_log_info "Cleaning the binlog directory $binlog_dir as well"
            rm -fv "$WSREP_SST_OPT_BINLOG".[0-9]* 1>&2 \+ || true
            binlog_index="${WSREP_SST_OPT_BINLOG_INDEX%.index}.index"
            [ -f "$binlog_index" ] && rm -fv "$binlog_index" 1>&2 \+ || true
            cd "$OLD_PWD"
        fi

        TDATA="${DATA}"
        DATA="${DATA}/.sst"

        MAGIC_FILE="${DATA}/${INFO_FILE}"
        wsrep_log_info "Waiting for SST streaming to complete!"
        monitor_process $jpid

        get_proc

        if [[ ! -s "${DATA}/xtrabackup_checkpoints" ]];then
            wsrep_log_error "xtrabackup_checkpoints missing, failed innobackupex/SST on donor"
            exit 2
        fi

        # Rebuild indexes for compact backups
        if grep -q 'compact = 1' "${DATA}/xtrabackup_checkpoints";then
            wsrep_log_info "Index compaction detected"
            rebuild=1
        fi

        if [[ $rebuild -eq 1 ]];then
            nthreads=$(parse_cnf xtrabackup rebuild-threads $nproc)
            wsrep_log_info "Rebuilding during prepare with $nthreads threads"
            rebuildcmd="--rebuild-indexes --rebuild-threads='$nthreads'"
        fi

        qpfiles=$(find "${DATA}" -maxdepth 1 -type f -name '*.qp' -print -quit)
        if [ -n "$qpfiles" ]; then
            wsrep_log_info "Compressed qpress files found"

            if [ -z "$(command -v qpress)" ]; then
                wsrep_log_error "qpress not found in path: $PATH"
                exit 22
            fi

            dcmd="xargs -n 2 qpress -dT$nproc"

            if [[ -n "$progress" ]] && pv --help | grep -qw '--line-mode';then
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

            # Decompress the qpress files
            wsrep_log_info "Decompression with $nproc threads"
            timeit "Joiner-Decompression" "find ${DATA} -type f -name '*.qp' -printf '%p\n%h\n' | $dcmd"
            extcode=$?

            if [[ $extcode -eq 0 ]];then
                wsrep_log_info "Removing qpress files after decompression"
                find "$DATA" -type f -name '*.qp' -delete
                if [[ $? -ne 0 ]];then
                    wsrep_log_error "Something went wrong with deletion of qpress files. Investigate"
                fi
            else
                wsrep_log_error "Decompression failed. Exit code: $extcode"
                exit 22
            fi
        fi

        if  [ -n "$WSREP_SST_OPT_BINLOG" ]; then

            BINLOG_DIRNAME=$(dirname "$WSREP_SST_OPT_BINLOG")
            BINLOG_FILENAME=$(basename "$WSREP_SST_OPT_BINLOG")

            # To avoid comparing data directory and BINLOG_DIRNAME
            mv "$DATA/$BINLOG_FILENAME".* "$BINLOG_DIRNAME/" 2>/dev/null || true

            cd "$BINLOG_DIRNAME"
            for bfile in $(ls -1 "$BINLOG_FILENAME".[0-9]*); do
                echo "$BINLOG_DIRNAME/$bfile" >> "${WSREP_SST_OPT_BINLOG_INDEX%.index}.index"
            done
            cd "$OLD_PWD"

        fi

        wsrep_log_info "Preparing the backup at ${DATA}"
        setup_commands
        timeit "Xtrabackup prepare stage" "$INNOAPPLY"

        if [ $? -ne 0 ];
        then
            wsrep_log_error "${INNOBACKUPEX_BIN} apply finished with errors. Check ${DATA}/innobackup.prepare.log"
            exit 22
        fi

        MAGIC_FILE="${TDATA}/${INFO_FILE}"
        set +e
        rm "$TDATA/innobackup.prepare.log" "$TDATA/innobackup.move.log"
        set -e
        wsrep_log_info "Moving the backup to ${TDATA}"
        timeit "Xtrabackup move stage" "$INNOMOVE"
        if [[ $? -eq 0 ]];then
            wsrep_log_info "Move successful, removing ${DATA}"
            rm -rf "$DATA"
            DATA="${TDATA}"
        else
            wsrep_log_error "Move failed, keeping ${DATA} for further diagnosis"
            wsrep_log_error "Check ${DATA}/innobackup.move.log for details"
            exit 22
        fi

    else
        wsrep_log_info "${IST_FILE} received from donor: Running IST"
    fi

    if [[ ! -r "${MAGIC_FILE}" ]];then
        wsrep_log_error "SST magic file ${MAGIC_FILE} not found/readable"
        exit 2
    fi

    coords=$(cat "$MAGIC_FILE")
    wsrep_log_info "Galera co-ords from recovery: $coords"
    cat "$MAGIC_FILE" # Output : UUID:seqno wsrep_gtid_domain_id

    wsrep_log_info "Total time on joiner: $totime seconds"
fi

exit 0
