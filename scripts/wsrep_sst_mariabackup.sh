#!/usr/bin/env bash

set -ue

# Copyright (C) 2017-2021 MariaDB
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
# MA  02110-1335  USA.

# Documentation:
# https://mariadb.com/kb/en/mariabackup-overview/
# Make sure to read that before proceeding!

OS="$(uname)"

. $(dirname "$0")/wsrep_sst_common
wsrep_check_datadir

ealgo=""
eformat=""
ekey=""
ekeyfile=""
encrypt=0
ssyslog=""
ssystag=""
BACKUP_PID=""
tcert=""
tcap=""
tpem=""
tkey=""
tmode=""
sockopt=""
progress=""
ttime=0
totime=0
lsn=""
ecmd=""
rlimit=""
# Initially
stagemsg="$WSREP_SST_OPT_ROLE"
cpat=""
speciald=1
ib_home_dir=""
ib_log_dir=""
ib_undo_dir=""

sfmt=""
strmcmd=""
tfmt=""
tcmd=""
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

compress='none'
compress_chunk=""
compress_threads=""

backup_threads=""

encrypt_threads=""
encrypt_chunk=""

readonly SECRET_TAG="secret"

# Required for backup locks
# For backup locks it is 1 sent by joiner
sst_ver=1

if [ -n "$(commandex pv)" ] && pv --help | grep -qw -- '-F'; then
    pvopts="$pvopts $pvformat"
fi
pcmd="pv $pvopts"
declare -a RC

BACKUP_BIN=$(commandex 'mariabackup')
if [ -z "$BACKUP_BIN" ]; then
    wsrep_log_error 'mariabackup binary not found in path'
    exit 42
fi

DATA="$WSREP_SST_OPT_DATA"
INFO_FILE="xtrabackup_galera_info"
IST_FILE="xtrabackup_ist"
MAGIC_FILE="$DATA/$INFO_FILE"

INNOAPPLYLOG="$DATA/mariabackup.prepare.log"
INNOMOVELOG="$DATA/mariabackup.move.log"
INNOBACKUPLOG="$DATA/mariabackup.backup.log"

# Setting the path for ss and ip
export PATH="/usr/sbin:/sbin:$PATH"

timeit()
{
    local stage="$1"
    shift
    local cmd="$@"
    local x1 x2 took extcode

    if [ $ttime -eq 1 ]; then
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
    if [ $encrypt -ge 2 -o $encrypt -eq -1 ]; then
        return
    fi

    if [ $encrypt -eq 0 ]; then
        if [ -n "$ealgo" -o -n "$ekey" -o -n "$ekeyfile" ]; then
            wsrep_log_error "Options for encryption are specified," \
                            "but encryption itself is disabled. SST may fail."
        fi
        return
    fi

    if [ $sfmt = 'tar' ]; then
        wsrep_log_info "NOTE: key-based encryption (encrypt=1)" \
                       "cannot be enabled with tar format"
        encrypt=-1
        return
    fi

    wsrep_log_info "Key based encryption enabled in my.cnf"

    if [ -z "$ealgo" ]; then
        wsrep_log_error "FATAL: Encryption algorithm empty from my.cnf, bailing out"
        exit 3
    fi

    if [ -z "$ekey" ]; then
        if [ ! -r "$ekeyfile" ]; then
            wsrep_log_error "FATAL: Either key must be specified" \
                            "or keyfile must be readable"
            exit 3
        fi
    fi

    if [ "$eformat" = 'openssl' ]; then
        get_openssl
        if [ -z "$OPENSSL_BINARY" ]; then
            wsrep_log_error "If encryption using the openssl is enabled," \
                            "then you need to install openssl"
            exit 2
        fi
        ecmd="'$OPENSSL_BINARY' enc -$ealgo"
        if "$OPENSSL_BINARY" enc -help 2>&1 | grep -qw -- '-pbkdf2'; then
            ecmd="$ecmd -pbkdf2"
        elif "$OPENSSL_BINARY" enc -help 2>&1 | grep -qw -- '-iter'; then
            ecmd="$ecmd -iter 1"
        elif "$OPENSSL_BINARY" enc -help 2>&1 | grep -qw -- '-md'; then
            ecmd="$ecmd -md sha256"
        fi
        if [ -z "$ekey" ]; then
            ecmd="$ecmd -kfile '$ekeyfile'"
        else
            ecmd="$ecmd -k '$ekey'"
        fi
    elif [ "$eformat" = 'xbcrypt' ]; then
        if [ -z "$(commandex xbcrypt)" ]; then
            wsrep_log_error "If encryption using the xbcrypt is enabled," \
                            "then you need to install xbcrypt"
            exit 2
        fi
        wsrep_log_info "NOTE: xbcrypt-based encryption," \
                       "supported only from Xtrabackup 2.1.4"
        if [ -z "$ekey" ]; then
            ecmd="xbcrypt --encrypt-algo='$ealgo' --encrypt-key-file='$ekeyfile'"
        else
            ecmd="xbcrypt --encrypt-algo='$ealgo' --encrypt-key='$ekey'"
        fi
        if [ -n "$encrypt_threads" ]; then
            ecmd="$ecmd --encrypt-threads=$encrypt_threads"
        fi
        if [ -n "$encrypt_chunk" ]; then
            ecmd="$ecmd --encrypt-chunk-size=$encrypt_chunk"
        fi
    else
        wsrep_log_error "Unknown encryption format='$eformat'"
        exit 2
    fi

    if [ "$WSREP_SST_OPT_ROLE" = 'joiner' ]; then
        ecmd="$ecmd -d"
    fi

    stagemsg="$stagemsg-XB-Encrypted"
}

get_transfer()
{
    if [ $tfmt = 'nc' ]; then
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
            tcmd="$tcmd $SST_PORT"
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
            tcmd="$tcmd $WSREP_SST_OPT_HOST_UNESCAPED $SST_PORT"
        fi
    else
        tfmt='socat'

        wsrep_log_info "Using socat as streamer"
        wsrep_check_programs socat

        if [ -n "$sockopt" ]; then
            sockopt=$(trim_string "$sockopt" ',')
            if [ -n "$sockopt" ]; then
                sockopt=",$sockopt"
            fi
        fi

        # Add an option for ipv6 if needed:
        if [ $WSREP_SST_OPT_HOST_IPv6 -eq 1 ]; then
            # If sockopt contains 'pf=ip6' somewhere in the middle,
            # this will not interfere with socat, but exclude the trivial
            # cases when sockopt contains 'pf=ip6' as prefix or suffix:
            if [ "$sockopt" = "${sockopt#,pf=ip6}" -a \
                 "$sockopt" = "${sockopt%,pf=ip6}" ]
            then
                sockopt=",pf=ip6$sockopt"
            fi
        fi

        if [ $encrypt -lt 2 ]; then
            if [ "$WSREP_SST_OPT_ROLE" = 'joiner' ]; then
                tcmd="socat -u TCP-LISTEN:$SST_PORT,reuseaddr$sockopt stdio"
            else
                tcmd="socat -u stdio TCP:$REMOTEIP:$SST_PORT$sockopt"
            fi
            return
        fi

        if ! socat -V | grep -q -F 'WITH_OPENSSL 1'; then
            wsrep_log_error "******** FATAL ERROR ************************************************ "
            wsrep_log_error "* Encryption requested, but socat is not OpenSSL enabled (encrypt=$encrypt) *"
            wsrep_log_error "********************************************************************* "
            exit 2
        fi

        local action='Decrypting'
        if [ "$WSREP_SST_OPT_ROLE" = 'joiner' ]; then
            tcmd="socat -u openssl-listen:$SST_PORT,reuseaddr"
        else
            tcmd="socat -u stdio openssl-connect:$REMOTEIP:$SST_PORT"
            action='Encrypting'
        fi

        if [ "${sockopt#*,dhparam=}" != "$sockopt" ]; then
            if [ -z "$ssl_dhparams" ]; then
                # Determine the socat version
                SOCAT_VERSION=$(socat -V 2>&1 | \
                                grep -m1 -owE '[0-9]+(\.[0-9]+)+' | head -n1)
                if [ -z "$SOCAT_VERSION" ]; then
                    wsrep_log_error "******** FATAL ERROR ******************"
                    wsrep_log_error "* Cannot determine the socat version. *"
                    wsrep_log_error "***************************************"
                    exit 2
                fi
                if ! check_for_version "$SOCAT_VERSION" '1.7.3'; then
                    # socat versions < 1.7.3 will have 512-bit dhparams (too small)
                    # so create 2048-bit dhparams and send that as a parameter:
                    check_for_dhparams
                fi
            fi
            if [ -n "$ssl_dhparams" ]; then
                tcmd="$tcmd,dhparam='$ssl_dhparams'"
            fi
        fi

        CN_option=",commonname=''"

        if [ $encrypt -eq 2 ]; then
            wsrep_log_info \
                "Using openssl based encryption with socat: with crt and pem"
            if [ -z "$tpem" -o -z "$tcert$tcap" ]; then
                wsrep_log_error \
                    "Both PEM file and CRT file (or path) are required"
                exit 22
            fi
            verify_ca_matches_cert "$tpem" "$tcert" "$tcap"
            tcmd="$tcmd,cert='$tpem'"
            if [ -n "$tcert" ]; then
                tcmd="$tcmd,cafile='$tcert'"
            fi
            if [ -n "$tcap" ]; then
                tcmd="$tcmd,capath='$tcap'"
            fi
            stagemsg="$stagemsg-OpenSSL-Encrypted-2"
            wsrep_log_info "$action with cert='$tpem', ca='$tcert', capath='$tcap'"
        elif [ $encrypt -eq 3 -o $encrypt -eq 4 ]; then
            wsrep_log_info \
                "Using openssl based encryption with socat: with key and crt"
            if [ -z "$tpem" -o -z "$tkey" ]; then
                wsrep_log_error "Both the certificate file (or path) and" \
                                "the key file are required"
                exit 22
            fi
            verify_cert_matches_key "$tpem" "$tkey"
            stagemsg="$stagemsg-OpenSSL-Encrypted-3"
            if [ -z "$tcert$tcap" ]; then
                if [ $encrypt -eq 4 ]; then
                    wsrep_log_error \
                        "Peer certificate file (or path) required if encrypt=4"
                    exit 22
                fi
                # no verification
                CN_option=""
                tcmd="$tcmd,cert='$tpem',key='$tkey',verify=0"
                wsrep_log_info \
                    "$action with cert='$tpem', key='$tkey', verify=0"
            else
                # CA verification
                verify_ca_matches_cert "$tpem" "$tcert" "$tcap"
                if [ -n "$WSREP_SST_OPT_REMOTE_USER" ]; then
                    CN_option=",commonname='$WSREP_SST_OPT_REMOTE_USER'"
                elif [ "$WSREP_SST_OPT_ROLE" = 'joiner' -o $encrypt -eq 4 ]
                then
                    CN_option=",commonname=''"
                elif is_local_ip "$WSREP_SST_OPT_HOST_UNESCAPED"; then
                    CN_option=',commonname=localhost'
                else
                    CN_option=",commonname='$WSREP_SST_OPT_HOST_UNESCAPED'"
                fi
                tcmd="$tcmd,cert='$tpem',key='$tkey'"
                if [ -n "$tcert" ]; then
                    tcmd="$tcmd,cafile='$tcert'"
                fi
                if [ -n "$tcap" ]; then
                    tcmd="$tcmd,capath='$tcap'"
                fi
                wsrep_log_info "$action with cert='$tpem', key='$tkey'," \
                               "ca='$tcert', capath='$tcap'"
            fi
        else
            wsrep_log_info "Unknown encryption mode: encrypt=$encrypt"
            exit 22
        fi

        tcmd="$tcmd$CN_option$sockopt"

        if [ "$WSREP_SST_OPT_ROLE" = 'joiner' ]; then
            tcmd="$tcmd stdio"
        fi
    fi
}

get_footprint()
{
    pushd "$WSREP_SST_OPT_DATA" 1>/dev/null
    payload=$(find . -regex '.*\.ibd$\|.*\.MYI$\|.*\.MYD$\|.*ibdata1$' \
              -type f -print0 | du --files0-from=- --block-size=1 -c -s | \
              awk 'END { print $1 }')
    if [ "$compress" != 'none' ]; then
        # QuickLZ has around 50% compression ratio
        # When compression/compaction used, the progress is only an approximate.
        payload=$(( payload*1/2 ))
    fi
    popd 1>/dev/null
    pcmd="$pcmd -s $payload"
    adjust_progress
}

adjust_progress()
{
    if [ -z "$(commandex pv)" ]; then
        wsrep_log_error "pv not found in path: $PATH"
        wsrep_log_error "Disabling all progress/rate-limiting"
        pcmd=""
        rlimit=""
        progress=""
        return
    fi

    if [ -n "$progress" -a "$progress" != '1' ]; then
        if [ -e "$progress" ]; then
            pcmd="$pcmd 2>>'$progress'"
        else
            pcmd="$pcmd 2>'$progress'"
        fi
    elif [ -z "$progress" -a -n "$rlimit" ]; then
            # When rlimit is non-zero
            pcmd="pv -q"
    fi

    if [ -n "$rlimit" -a "$WSREP_SST_OPT_ROLE" = 'donor' ]; then
        wsrep_log_info "Rate-limiting SST to $rlimit"
        pcmd="$pcmd -L \$rlimit"
    fi
}

encgroups='--mysqld|sst|xtrabackup'

read_cnf()
{
    sfmt=$(parse_cnf sst streamfmt 'mbstream')
    tfmt=$(parse_cnf sst transferfmt 'socat')

    encrypt=$(parse_cnf "$encgroups" 'encrypt' 0)
    tmode=$(parse_cnf "$encgroups" 'ssl-mode' 'DISABLED' | \
            tr [:lower:] [:upper:])

    case "$tmode" in
    'VERIFY_IDENTITY'|'VERIFY_CA'|'REQUIRED'|'DISABLED')
        ;;
    *)
        wsrep_log_error "Unrecognized ssl-mode option: '$tmode'"
        exit 22 # EINVAL
        ;;
    esac

    if [ $encrypt -eq 0 -o $encrypt -ge 2 ]; then
        if [ "$tmode" != 'DISABLED' -o $encrypt -ge 2 ]; then
            check_server_ssl_config
        fi
        if [ "$tmode" != 'DISABLED' ]; then
            if [ 0 -eq $encrypt -a -n "$tpem" -a -n "$tkey" ]
            then
                encrypt=3 # enable cert/key SSL encyption
                # avoid CA verification if not set explicitly:
                # nodes may happen to have different CA if self-generated,
                # zeroing up tcert and tcap does the trick:
                if [ "${tmode#VERIFY}" = "$tmode" ]; then
                    tcert=""
                    tcap=""
                fi
            fi
        fi
    elif [ $encrypt -eq 1 ]; then
        ealgo=$(parse_cnf "$encgroups" 'encrypt-algo')
        eformat=$(parse_cnf "$encgroups" 'encrypt-format' 'openssl')
        ekey=$(parse_cnf "$encgroups" 'encrypt-key')
        # The keyfile should be read only when the key
        # is not specified or empty:
        if [ -z "$ekey" ]; then
            ekeyfile=$(parse_cnf "$encgroups" 'encrypt-key-file')
        fi
    fi

    wsrep_log_info "SSL configuration: CA='$tcert', CAPATH='$tcap'," \
                   "CERT='$tpem', KEY='$tkey', MODE='$tmode'," \
                   "encrypt='$encrypt'"

    sockopt=$(parse_cnf sst sockopt "")
    progress=$(parse_cnf sst progress "")
    ttime=$(parse_cnf sst time 0)
    cpat='.*\.pem$\|.*galera\.cache$\|.*sst_in_progress$\|.*\.sst$\|.*gvwstate\.dat$\|.*grastate\.dat$\|.*\.err$\|.*\.log$\|.*RPM_UPGRADE_MARKER$\|.*RPM_UPGRADE_HISTORY$'
    [ "$OS" = 'FreeBSD' ] && cpat=$(echo "$cpat" | sed 's/\\|/|/g')
    cpat=$(parse_cnf sst cpat "$cpat")
    scomp=$(parse_cnf sst compressor "")
    sdecomp=$(parse_cnf sst decompressor "")

    rlimit=$(parse_cnf sst rlimit "")
    uextra=$(parse_cnf sst use-extra 0)
    speciald=$(parse_cnf sst sst-special-dirs 1)
    iopts=$(parse_cnf sst inno-backup-opts "")
    iapts=$(parse_cnf sst inno-apply-opts "")
    impts=$(parse_cnf sst inno-move-opts "")
    stimeout=$(parse_cnf sst sst-initial-timeout 300)
    ssyslog=$(parse_cnf sst sst-syslog 0)
    ssystag=$(parse_cnf mysqld_safe syslog-tag "${SST_SYSLOG_TAG:-}")
    ssystag="$ssystag-"
    sstlogarchive=$(parse_cnf sst sst-log-archive 1)
    sstlogarchivedir=$(parse_cnf sst sst-log-archive-dir '/tmp/sst_log_archive')

    if [ $speciald -eq 0 ]; then
        wsrep_log_error \
            "sst-special-dirs equal to 0 is not supported, falling back to 1"
        speciald=1
    fi

    if [ $ssyslog -ne -1 ]; then
        ssyslog=$(in_config 'mysqld_safe' 'syslog')
    fi

    if [ "$WSREP_SST_OPT_ROLE" = 'donor' ]; then
        compress=$(parse_cnf "$encgroups" 'compress' 'none')
        if [ "$compress" != 'none' ]; then
            compress_chunk=$(parse_cnf "$encgroups" 'compress-chunk-size')
            compress_threads=$(parse_cnf "$encgroups" 'compress-threads')
        fi
    fi

    backup_threads=$(parse_cnf "$encgroups" 'backup-threads')

    if [ "$eformat" = 'xbcrypt' ]; then
        encrypt_threads=$(parse_cnf "$encgroups" 'encrypt-threads')
        encrypt_chunk=$(parse_cnf "$encgroups" 'encrypt-chunk-size')
    fi
}

get_stream()
{
    if [ "$sfmt" = 'mbstream' -o "$sfmt" = 'xbstream' ]; then
        sfmt='mbstream'
        local STREAM_BIN=$(commandex "$sfmt")
        if [ -z "$STREAM_BIN" ]; then
            wsrep_log_error "Streaming with $sfmt, but $sfmt not found in path"
            exit 42
        fi
        if [ "$WSREP_SST_OPT_ROLE" = 'joiner' ]; then
            strmcmd="'$STREAM_BIN' -x"
        else
            strmcmd="'$STREAM_BIN' -c '$INFO_FILE'"
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

sig_joiner_cleanup()
{
    wsrep_log_error "Removing $MAGIC_FILE file due to signal"
    [ -f "$MAGIC_FILE" ] && rm -f "$MAGIC_FILE"
}

cleanup_at_exit()
{
    # Since this is invoked just after exit NNN
    local estatus=$?
    if [ $estatus -ne 0 ]; then
        wsrep_log_error "Cleanup after exit with status: $estatus"
    fi

    if [ "$WSREP_SST_OPT_ROLE" = 'joiner' ]; then
        wsrep_log_info "Removing the sst_in_progress file"
        wsrep_cleanup_progress_file
    else
        if [ -n "$BACKUP_PID" ]; then
            if check_pid "$BACKUP_PID" 1; then
                wsrep_log_error \
                    "mariabackup process is still running. Killing..."
                cleanup_pid $CHECK_PID "$BACKUP_PID"
            fi
        fi
        [ -f "$DATA/$IST_FILE" ] && rm -f "$DATA/$IST_FILE" || :
    fi

    if [ -n "$progress" -a -p "$progress" ]; then
        wsrep_log_info "Cleaning up fifo file: $progress"
        rm -f "$progress" || :
    fi

    wsrep_log_info "Cleaning up temporary directories"

    if [ "$WSREP_SST_OPT_ROLE" = 'joiner' ]; then
        [ -n "$STATDIR" -a -d "$STATDIR" ] && rm -rf "$STATDIR" || :
    else
        [ -n "$xtmpdir" -a -d "$xtmpdir" ] && rm -rf "$xtmpdir" || :
        [ -n "$itmpdir" -a -d "$itmpdir" ] && rm -rf "$itmpdir" || :
    fi

    # Final cleanup
    pgid=$(ps -o pgid= $$ 2>/dev/null | grep -o '[0-9]*' || :)

    # This means no setsid done in mysqld.
    # We don't want to kill mysqld here otherwise.
    if [ -n "$pgid" ]; then
        if [ $$ -eq $pgid ]; then
            # This means a signal was delivered to the process.
            # So, more cleanup.
            if [ $estatus -ge 128 ]; then
                kill -KILL -- -$$ || :
            fi
        fi
    fi

    if [ -n "${SST_PID:-}" ]; then
        [ -f "$SST_PID" ] && rm -f "$SST_PID" || :
    fi

    exit $estatus
}

setup_ports()
{
    SST_PORT="$WSREP_SST_OPT_PORT"
    if [ "$WSREP_SST_OPT_ROLE" = 'donor' ]; then
        REMOTEIP="$WSREP_SST_OPT_HOST"
        lsn="$WSREP_SST_OPT_LSN"
        sst_ver="$WSREP_SST_OPT_SST_VER"
    fi
}

#
# Waits ~30 seconds for socat or nc to open the port and
# then reports ready, regardless of timeout.
#
wait_for_listen()
{
    for i in {1..150}; do
        if check_port "" "$SST_PORT" 'socat|nc'; then
            break
        fi
        sleep 0.2
    done
    echo "ready $ADDR:$SST_PORT/$MODULE/$lsn/$sst_ver"
}

check_extra()
{
    local use_socket=1
    if [ $uextra -eq 1 ]; then
        local thread_handling=$(parse_cnf '--mysqld' 'thread-handling')
        if [ "$thread_handling" = 'pool-of-threads' ]; then
            local eport=$(parse_cnf '--mysqld' 'extra-port')
            if [ -n "$eport" ]; then
                # mariabackup works only locally.
                # Hence, setting host to 127.0.0.1 unconditionally:
                wsrep_log_info "SST through extra_port $eport"
                INNOEXTRA="$INNOEXTRA --host=127.0.0.1 --port=$eport"
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
        INNOEXTRA="$INNOEXTRA --socket='$WSREP_SST_OPT_SOCKET'"
    fi
}

recv_joiner()
{
    local dir="$1"
    local msg="$2"
    local tmt=$3
    local checkf=$4
    local wait=$5

    if [ ! -d "$dir" ]; then
        # This indicates that IST is in progress
        return
    fi

    local ltcmd="$tcmd"
    if [ $tmt -gt 0 ]; then
        if [ -n "$(commandex timeout)" ]; then
            if timeout --help | grep -qw -- '-k'; then
                ltcmd="timeout -k $(( tmt+10 )) $tmt $tcmd"
            else
                ltcmd="timeout -s9 $tmt $tcmd"
            fi
        fi
    fi

    pushd "$dir" 1>/dev/null
    set +e

    if [ $wait -ne 0 ]; then
        wait_for_listen &
    fi

    timeit "$msg" "$ltcmd | $strmcmd; RC=( "\${PIPESTATUS[@]}" )"

    set -e
    popd 1>/dev/null

    if [ ${RC[0]} -eq 124 ]; then
        wsrep_log_error "Possible timeout in receiving first data from" \
                        "donor in gtid stage: exit codes: ${RC[@]}"
        exit 32
    fi

    for ecode in "${RC[@]}"; do
        if [ $ecode -ne 0 ]; then
            wsrep_log_error "Error while getting data from donor node:" \
                            "exit codes: ${RC[@]}"
            exit 32
        fi
    done

    if [ $checkf -eq 1 ]; then
        if [ ! -r "$MAGIC_FILE" ]; then
            # this message should cause joiner to abort
            wsrep_log_error "receiving process ended without creating" \
                            "'$MAGIC_FILE'"
            wsrep_log_info "Contents of datadir"
            wsrep_log_info $(ls -l "$dir/"*)
            exit 32
        fi

        # check donor supplied secret
        SECRET=$(grep -F -- "$SECRET_TAG " "$MAGIC_FILE" 2>/dev/null | \
                 cut -d ' ' -f2)
        if [ "$SECRET" != "$MY_SECRET" ]; then
            wsrep_log_error "Donor does not know my secret!"
            wsrep_log_info "Donor: '$SECRET', my: '$MY_SECRET'"
            exit 32
        fi

        # remove secret from the magic file
        grep -v -F -- "$SECRET_TAG " "$MAGIC_FILE" > "$MAGIC_FILE.new"
        mv "$MAGIC_FILE.new" "$MAGIC_FILE"
    fi
}

send_donor()
{
    local dir="$1"
    local msg="$2"

    pushd "$dir" 1>/dev/null
    set +e
    timeit "$msg" "$strmcmd | $tcmd; RC=( "\${PIPESTATUS[@]}" )"
    set -e
    popd 1>/dev/null

    for ecode in "${RC[@]}"; do
        if [ $ecode -ne 0 ]; then
            wsrep_log_error "Error while sending data to joiner node:" \
                            "exit codes: ${RC[@]}"
            exit 32
        fi
    done
}

monitor_process()
{
    local sst_stream_pid=$1

    while true ; do
        if ! ps -p "$WSREP_SST_OPT_PARENT" >/dev/null 2>&1; then
            wsrep_log_error \
                "Parent mysqld process (PID: $WSREP_SST_OPT_PARENT)" \
                "terminated unexpectedly."
            kill -- -"$WSREP_SST_OPT_PARENT"
            exit 32
        fi
        if ! ps -p "$sst_stream_pid" >/dev/null 2>&1; then
            break
        fi
        sleep 0.1
    done
}

[ -f "$MAGIC_FILE" ] && rm -f "$MAGIC_FILE"

if [ "$WSREP_SST_OPT_ROLE" != 'joiner' -a "$WSREP_SST_OPT_ROLE" != 'donor' ]; then
    wsrep_log_error "Invalid role '$WSREP_SST_OPT_ROLE'"
    exit 22
fi

read_cnf
setup_ports

if "$BACKUP_BIN" --help 2>/dev/null | grep -qw -- '--version-check'; then
    disver=' --no-version-check'
fi

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

if [ $ssyslog -eq 1 ]; then
    if [ -n "$(commandex logger)" ]; then
        wsrep_log_info "Logging all stderr of SST/mariabackup to syslog"

        exec 2> >(logger -p daemon.err -t ${ssystag}wsrep-sst-$WSREP_SST_OPT_ROLE)

        wsrep_log_error()
        {
            logger -p daemon.err -t ${ssystag}wsrep-sst-$WSREP_SST_OPT_ROLE "$@"
        }

        wsrep_log_info()
        {
            logger -p daemon.info -t ${ssystag}wsrep-sst-$WSREP_SST_OPT_ROLE "$@"
        }
    else
        wsrep_log_error "logger not in path: $PATH. Ignoring"
    fi
    INNOAPPLY="2>&1 | logger -p daemon.err -t ${ssystag}innobackupex-apply"
    INNOMOVE="2>&1 | logger -p daemon.err -t ${ssystag}innobackupex-move"
    INNOBACKUP="2> >(logger -p daemon.err -t ${ssystag}innobackupex-backup)"
else
    if [ $sstlogarchive -eq 1 ]
    then
        ARCHIVETIMESTAMP=$(date "+%Y.%m.%d-%H.%M.%S.%N")

        if [ -n "$sstlogarchivedir" ]; then
            if [ ! -d "$sstlogarchivedir" ]; then
                mkdir -p "$sstlogarchivedir"
            fi
        fi

        if [ -e "$INNOAPPLYLOG" ]; then
            if [ -n "$sstlogarchivedir" ]; then
                newfile=$(basename "$INNOAPPLYLOG")
                newfile="$sstlogarchivedir/$newfile.$ARCHIVETIMESTAMP"
            else
                newfile="$INNOAPPLYLOG.$ARCHIVETIMESTAMP"
            fi
            wsrep_log_info "Moving '$INNOAPPLYLOG' to '$newfile'"
            mv "$INNOAPPLYLOG" "$newfile"
            gzip "$newfile"
        fi

        if [ -e "$INNOMOVELOG" ]; then
            if [ -n "$sstlogarchivedir" ]; then
                newfile=$(basename "$INNOMOVELOG")
                newfile="$sstlogarchivedir/$newfile.$ARCHIVETIMESTAMP"
            else
                newfile="$INNOMOVELOG.$ARCHIVETIMESTAMP"
            fi
            wsrep_log_info "Moving '$INNOMOVELOG' to '$newfile'"
            mv "$INNOMOVELOG" "$newfile"
            gzip "$newfile"
        fi

        if [ -e "$INNOBACKUPLOG" ]; then
            if [ -n "$sstlogarchivedir" ]; then
                newfile=$(basename "$INNOBACKUPLOG")
                newfile="$sstlogarchivedir/$newfile.$ARCHIVETIMESTAMP"
            else
                newfile="$INNOBACKUPLOG.$ARCHIVETIMESTAMP"
            fi
            wsrep_log_info "Moving '$INNOBACKUPLOG' to '$newfile'"
            mv "$INNOBACKUPLOG" "$newfile"
            gzip "$newfile"
        fi
    fi
    INNOAPPLY="> '$INNOAPPLYLOG' 2>&1"
    INNOMOVE="> '$INNOMOVELOG' 2>&1"
    INNOBACKUP="2> '$INNOBACKUPLOG'"
fi

setup_commands()
{
    local mysqld_args=""
    if [ -n "$WSREP_SST_OPT_MYSQLD" ]; then
        mysqld_args=" --mysqld-args $WSREP_SST_OPT_MYSQLD"
    fi
    local recovery=""
    if [ -n "$INNODB_FORCE_RECOVERY" ]; then
        recovery=" --innodb-force-recovery=$INNODB_FORCE_RECOVERY"
    fi
    INNOAPPLY="$BACKUP_BIN --prepare$disver$recovery${iapts:+ }$iapts$INNOEXTRA --target-dir='$DATA' --datadir='$DATA'$mysqld_args $INNOAPPLY"
    INNOMOVE="$BACKUP_BIN$WSREP_SST_OPT_CONF --move-back$disver${impts:+ }$impts --force-non-empty-directories --target-dir='$DATA' --datadir='${TDATA:-$DATA}' $INNOMOVE"
    INNOBACKUP="$BACKUP_BIN$WSREP_SST_OPT_CONF --backup$disver${iopts:+ }$iopts$tmpopts$INNOEXTRA --galera-info --stream=$sfmt --target-dir='$itmpdir' --datadir='$DATA'$mysqld_args $INNOBACKUP"
}

get_stream
get_transfer

if [ "$WSREP_SST_OPT_ROLE" = 'donor' ]
then
    trap cleanup_at_exit EXIT

    if [ $WSREP_SST_OPT_BYPASS -eq 0 ]
    then
        if [ -z "$sst_ver" ]; then
            wsrep_log_error "Upgrade joiner to 5.6.21 or higher for backup locks support"
            wsrep_log_error "The joiner is not supported for this version of donor"
            exit 93
        fi

        tmpdir=$(parse_cnf "$encgroups" 'tmpdir')
        if [ -z "$tmpdir" ]; then
            xtmpdir="$(mktemp -d)"
        elif [ "$OS" = 'Linux' ]; then
            xtmpdir=$(mktemp '-d' "--tmpdir=$tmpdir")
        else
            xtmpdir=$(TMPDIR="$tmpdir"; mktemp '-d')
        fi

        wsrep_log_info "Using '$xtmpdir' as mariabackup temporary directory"
        tmpopts=" --tmpdir='$xtmpdir'"

        itmpdir="$(mktemp -d)"
        wsrep_log_info "Using '$itmpdir' as mariabackup working directory"

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

        check_extra

        wsrep_log_info "Streaming GTID file before SST"

        # Store donor's wsrep GTID (state ID) and wsrep_gtid_domain_id
        # (separated by a space).
        echo "$WSREP_SST_OPT_GTID $WSREP_SST_OPT_GTID_DOMAIN_ID" > "$MAGIC_FILE"

        if [ -n "$WSREP_SST_OPT_REMOTE_PSWD" ]; then
            # Let joiner know that we know its secret
            echo "$SECRET_TAG $WSREP_SST_OPT_REMOTE_PSWD" >> "$MAGIC_FILE"
        fi

        ttcmd="$tcmd"

        if [ -n "$scomp" ]; then
            tcmd="$scomp | $tcmd"
        fi

        get_keys
        if [ $encrypt -eq 1 ]; then
            tcmd="$ecmd | $tcmd"
        fi

        send_donor "$DATA" "$stagemsg-gtid"

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

        wsrep_log_info "Streaming the backup to joiner at $REMOTEIP:$SST_PORT"

        # Add compression to the head of the stream (if specified)
        if [ -n "$scomp" ]; then
            tcmd="$scomp | $tcmd"
        fi

        # Add encryption to the head of the stream (if specified)
        if [ $encrypt -eq 1 ]; then
            tcmd="$ecmd | $tcmd"
        fi

        iopts="--databases-exclude='lost+found'${iopts:+ }$iopts"

        if [ ${FORCE_FTWRL:-0} -eq 1 ]; then
            wsrep_log_info "Forcing FTWRL due to environment variable" \
                           "FORCE_FTWRL equal to $FORCE_FTWRL"
            iopts="--no-backup-locks${iopts:+ }$iopts"
        fi

        # if compression is enabled for backup files, then add the
        # appropriate options to the mariabackup command line:
        if [ "$compress" != 'none' ]; then
            iopts="--compress${compress:+=$compress}${iopts:+ }$iopts"
            if [ -n "$compress_threads" ]; then
                iopts="--compress-threads=$compress_threads${iopts:+ }$iopts"
            fi
            if [ -n "$compress_chunk" ]; then
                iopts="--compress-chunk-size=$compress_chunk${iopts:+ }$iopts"
            fi
        fi

        if [ -n "$backup_threads" ]; then
            iopts="--parallel=$backup_threads${iopts:+ }$iopts"
        fi

        setup_commands
        set +e
        timeit "$stagemsg-SST" "$INNOBACKUP | $tcmd; RC=( "\${PIPESTATUS[@]}" )"
        set -e

        if [ ${RC[0]} -ne 0 ]; then
            wsrep_log_error "mariabackup finished with error: ${RC[0]}." \
                            "Check syslog or '$INNOBACKUPLOG' for details"
            exit 22
        elif [ ${RC[$(( ${#RC[@]}-1 ))]} -eq 1 ]; then
            wsrep_log_error "$tcmd finished with error: ${RC[1]}"
            exit 22
        fi

        # mariabackup implicitly writes PID to fixed location in $xtmpdir
        BACKUP_PID="$xtmpdir/xtrabackup_pid"

    else # BYPASS FOR IST

        wsrep_log_info "Bypassing the SST for IST"
        echo "continue" # now server can resume updating data

        # Store donor's wsrep GTID (state ID) and wsrep_gtid_domain_id
        # (separated by a space).
        echo "$WSREP_SST_OPT_GTID $WSREP_SST_OPT_GTID_DOMAIN_ID" > "$MAGIC_FILE"
        echo "1" > "$DATA/$IST_FILE"

        if [ -n "$scomp" ]; then
            tcmd="$scomp | $tcmd"
        fi

        get_keys
        if [ $encrypt -eq 1 ]; then
            tcmd="$ecmd | $tcmd"
        fi

        strmcmd="$strmcmd '$IST_FILE'"

        send_donor "$DATA" "$stagemsg-IST"

    fi

    echo "done $WSREP_SST_OPT_GTID"
    wsrep_log_info "Total time on donor: $totime seconds"

elif [ "$WSREP_SST_OPT_ROLE" = 'joiner' ]
then
    [ -e "$SST_PROGRESS_FILE" ] && \
        wsrep_log_info "Stale sst_in_progress file: $SST_PROGRESS_FILE"
    [ -n "$SST_PROGRESS_FILE" ] && touch "$SST_PROGRESS_FILE"

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

    if [ -n "$backup_threads" ]; then
        impts="--parallel=$backup_threads${impts:+ }$impts"
    fi

    SST_PID="$WSREP_SST_OPT_DATA/wsrep_sst.pid"

    # give some time for previous SST to complete:
    check_round=0
    while check_pid "$SST_PID" 0; do
        wsrep_log_info "previous SST is not completed, waiting for it to exit"
        check_round=$(( check_round + 1 ))
        if [ $check_round -eq 10 ]; then
            wsrep_log_error "previous SST script still running."
            exit 114 # EALREADY
        fi
        sleep 1
    done

    echo $$ > "$SST_PID"

    stagemsg='Joiner-Recv'

    MODULE="${WSREP_SST_OPT_MODULE:-xtrabackup_sst}"

    [ -f "$DATA/$IST_FILE" ] && rm -f "$DATA/$IST_FILE"

    # May need xtrabackup_checkpoints later on
    [ -f "$DATA/xtrabackup_binary"      ] && rm -f "$DATA/xtrabackup_binary"
    [ -f "$DATA/xtrabackup_galera_info" ] && rm -f "$DATA/xtrabackup_galera_info"
    [ -f "$DATA/ib_logfile0"            ] && rm -f "$DATA/ib_logfile0"

    ADDR="$WSREP_SST_OPT_HOST"

    if [ "${tmode#VERIFY}" != "$tmode" ]; then
        # backward-incompatible behavior:
        CN=""
        if [ -n "$tpem" ]; then
            # find out my Common Name
            get_openssl
            if [ -z "$OPENSSL_BINARY" ]; then
                wsrep_log_error \
                    'openssl not found but it is required for authentication'
                exit 42
            fi
            CN=$("$OPENSSL_BINARY" x509 -noout -subject -in "$tpem" | \
                 tr "," "\n" | grep -F 'CN =' | cut -d '=' -f2 | sed s/^\ // | \
                 sed s/\ %//)
        fi
        MY_SECRET="$(wsrep_gen_secret)"
        # Add authentication data to address
        ADDR="$CN:$MY_SECRET@$ADDR"
    else
        MY_SECRET="" # for check down in recv_joiner()
    fi

    trap sig_joiner_cleanup HUP PIPE INT TERM
    trap cleanup_at_exit EXIT

    if [ -n "$progress" ]; then
        adjust_progress
        tcmd="$tcmd | $pcmd"
    fi

    get_keys
    if [ $encrypt -eq 1 ]; then
        strmcmd="$ecmd | $strmcmd"
    fi

    if [ -n "$sdecomp" ]; then
        strmcmd="$sdecomp | $strmcmd"
    fi

    check_sockets_utils

    STATDIR="$(mktemp -d)"
    MAGIC_FILE="$STATDIR/$INFO_FILE"

    recv_joiner "$STATDIR" "$stagemsg-gtid" $stimeout 1 1

    if ! ps -p "$WSREP_SST_OPT_PARENT" >/dev/null 2>&1
    then
        wsrep_log_error "Parent mysqld process (PID: $WSREP_SST_OPT_PARENT)" \
                        "terminated unexpectedly."
        exit 32
    fi

    if [ ! -r "$STATDIR/$IST_FILE" ]; then

        if [ -d "$DATA/.sst" ]; then
            wsrep_log_info \
                "WARNING: Stale temporary SST directory:" \
                "'$DATA/.sst' from previous state transfer, removing..."
            rm -rf "$DATA/.sst"
        fi
        mkdir -p "$DATA/.sst"
        (recv_joiner "$DATA/.sst" "$stagemsg-SST" 0 0 0) &
        jpid=$!
        wsrep_log_info "Proceeding with SST"

        wsrep_log_info \
            "Cleaning the existing datadir and innodb-data/log directories"
        if [ "$OS" = 'FreeBSD' ]; then
            find -E ${ib_home_dir:+"$ib_home_dir"} \
                    ${ib_undo_dir:+"$ib_undo_dir"} \
                    ${ib_log_dir:+"$ib_log_dir"} \
                    "$DATA" -mindepth 1 -prune -regex "$cpat" \
                    -o -exec rm -rfv {} 1>&2 \+
        else
            find ${ib_home_dir:+"$ib_home_dir"} \
                 ${ib_undo_dir:+"$ib_undo_dir"} \
                 ${ib_log_dir:+"$ib_log_dir"} \
                 "$DATA" -mindepth 1 -prune -regex "$cpat" \
                 -o -exec rm -rfv {} 1>&2 \+
        fi

        get_binlog

        if [ -n "$WSREP_SST_OPT_BINLOG" ]; then
            binlog_dir=$(dirname "$WSREP_SST_OPT_BINLOG")
            if [ -d "$binlog_dir" ]; then
                cd "$binlog_dir"
                wsrep_log_info "Cleaning the binlog directory $binlog_dir as well"
                rm -fv "$WSREP_SST_OPT_BINLOG".[0-9]* 1>&2 \+ || :
                [ -f "$WSREP_SST_OPT_BINLOG_INDEX" ] && \
                    rm -fv "$WSREP_SST_OPT_BINLOG_INDEX" 1>&2 \+
                cd "$OLD_PWD"
            fi
        fi

        TDATA="$DATA"
        DATA="$DATA/.sst"

        MAGIC_FILE="$DATA/$INFO_FILE"
        wsrep_log_info "Waiting for SST streaming to complete!"
        monitor_process $jpid

        if [ ! -s "$DATA/xtrabackup_checkpoints" ]; then
            wsrep_log_error "xtrabackup_checkpoints missing," \
                            "failed mariabackup/SST on donor"
            exit 2
        fi

        # Compact backups are not supported by mariabackup
        if grep -qw -F 'compact = 1' "$DATA/xtrabackup_checkpoints"; then
            wsrep_log_info "Index compaction detected"
            wsrel_log_error "Compact backups are not supported by mariabackup"
            exit 2
        fi

        qpfiles=$(find "$DATA" -maxdepth 1 -type f -name '*.qp' -print -quit)
        if [ -n "$qpfiles" ]; then
            wsrep_log_info "Compressed qpress files found"

            if [ -z "$(commandex qpress)" ]; then
                wsrep_log_error "qpress utility not found in the path"
                exit 22
            fi

            get_proc

            dcmd="xargs -n 2 qpress -dT$nproc"

            if [ -n "$progress" ] && pv --help | grep -qw -- '--line-mode'; then
                count=$(find "$DATA" -type f -name '*.qp' | wc -l)
                count=$(( count*2 ))
                pvopts="-f -s $count -l -N Decompression"
                if pv --help | grep -qw -- '-F'; then
                    pvopts="$pvopts -F '%N => Rate:%r Elapsed:%t %e Progress: [%b/$count]'"
                fi
                pcmd="pv $pvopts"
                adjust_progress
                dcmd="$pcmd | $dcmd"
            fi

            # Decompress the qpress files
            wsrep_log_info "Decompression with $nproc threads"
            timeit "Joiner-Decompression" \
                   "find '$DATA' -type f -name '*.qp' -printf '%p\n%h\n' | $dcmd"
            extcode=$?

            if [ $extcode -eq 0 ]; then
                wsrep_log_info "Removing qpress files after decompression"
                find "$DATA" -type f -name '*.qp' -delete
                if [ $? -ne 0 ]; then
                    wsrep_log_error \
                        "Something went wrong with deletion of qpress files." \
                        "Investigate"
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
            mv "$DATA/$BINLOG_FILENAME".* "$BINLOG_DIRNAME/" 2>/dev/null || :

            cd "$BINLOG_DIRNAME"
            for bfile in $(ls -1 "$BINLOG_FILENAME".[0-9]*); do
                echo "$BINLOG_DIRNAME/$bfile" >> "$WSREP_SST_OPT_BINLOG_INDEX"
            done
            cd "$OLD_PWD"

        fi

        wsrep_log_info "Preparing the backup at $DATA"
        setup_commands
        timeit "mariabackup prepare stage" "$INNOAPPLY"

        if [ $? -ne 0 ]; then
            wsrep_log_error "mariabackup apply finished with errors." \
                            "Check syslog or '$INNOAPPLYLOG' for details."
            exit 22
        fi

        MAGIC_FILE="$TDATA/$INFO_FILE"

        wsrep_log_info "Moving the backup to $TDATA"
        timeit "mariabackup move stage" "$INNOMOVE"
        if [ $? -eq 0 ]; then
            wsrep_log_info "Move successful, removing $DATA"
            rm -rf "$DATA"
            DATA="$TDATA"
        else
            wsrep_log_error "Move failed, keeping '$DATA' for further diagnosis"
            wsrep_log_error "Check syslog or '$INNOMOVELOG' for details"
            exit 22
        fi

    else

        wsrep_log_info "'$IST_FILE' received from donor: Running IST"

    fi

    if [ ! -r "$MAGIC_FILE" ]; then
        wsrep_log_error "SST magic file '$MAGIC_FILE' not found/readable"
        exit 2
    fi

    coords=$(cat "$MAGIC_FILE")
    wsrep_log_info "Galera co-ords from recovery: $coords"
    cat "$MAGIC_FILE" # Output : UUID:seqno wsrep_gtid_domain_id

    wsrep_log_info "Total time on joiner: $totime seconds"
fi

exit 0
