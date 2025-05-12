#!/usr/bin/env bash

set -ue

# Copyright (C) 2017-2024 MariaDB
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

# This is a reference script for mariadb-backup-based state snapshot transfer.

# Documentation:
# https://mariadb.com/kb/en/mariabackup-overview/
# Make sure to read that before proceeding!

. $(dirname "$0")/wsrep_sst_common

BACKUP_BIN=$(commandex 'mariadb-backup')
if [ -z "$BACKUP_BIN" ]; then
    wsrep_log_error 'mariadb-backup binary not found in path'
    exit 42
fi

BACKUP_PID=""

INFO_FILE='mariadb_backup_galera_info'
DONOR_INFO_FILE='donor_galera_info'
IST_FILE='xtrabackup_ist'

MAGIC_FILE="$DATA/$INFO_FILE"
DONOR_MAGIC_FILE="$DATA/$DONOR_INFO_FILE"

ealgo=""
eformat=""
ekey=""
ekeyfile=""
encrypt=0
ssyslog=""
ssystag=""
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
ar_log_dir=""

sfmt=""
strmcmd=""
tfmt=""
tcmd=""
payload=0
pvformat="-F '%N => Rate:%r Avg:%a Elapsed:%t %e Bytes: %b %p'"
pvopts="-f -i 10 -N $WSREP_SST_OPT_ROLE"
uextra=0
disver=""

STATDIR=""
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

readonly SECRET_TAG='secret'
readonly TOTAL_TAG='total'

# Required for backup locks
# For backup locks it is 1 sent by joiner
sst_ver=1

INNOAPPLYLOG="$DATA/mariabackup.prepare.log"
INNOMOVELOG="$DATA/mariabackup.move.log"
INNOBACKUPLOG="$DATA/mariabackup.backup.log"

timeit()
{
    local stage="$1"
    shift
    local cmd="$@"
    local x1 x2 took extcode

    if [ $ttime -eq 1 ]; then
        x1=$(date +%s)
    fi

    wsrep_log_info "Evaluating $cmd"
    eval $cmd
    extcode=$?

    if [ $ttime -eq 1 ]; then
        x2=$(date +%s)
        took=$(( x2-x1 ))
        wsrep_log_info "NOTE: $stage took $took seconds"
        totime=$(( totime+took ))
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

    if [ "$sfmt" = 'tar' ]; then
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

    if [ -z "$ekey" -a ! -r "$ekeyfile" ]; then
        wsrep_log_error "FATAL: Either key must be specified" \
                        "or keyfile must be readable"
        exit 3
    fi

    if [ "$eformat" = 'openssl' ]; then
        get_openssl
        if [ -z "$OPENSSL_BINARY" ]; then
            wsrep_log_error "If encryption using the openssl is enabled," \
                            "then you need to install openssl"
            exit 2
        fi
        ecmd="'$OPENSSL_BINARY' enc -$ealgo"
        if "$OPENSSL_BINARY" enc -help 2>&1 | grep -qw -F -- '-pbkdf2'; then
            ecmd="$ecmd -pbkdf2"
        elif "$OPENSSL_BINARY" enc -help 2>&1 | grep -qw -F -- '-iter'; then
            ecmd="$ecmd -iter 1"
        elif "$OPENSSL_BINARY" enc -help 2>&1 | grep -qw -F -- '-md'; then
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

    [ "$WSREP_SST_OPT_ROLE" = 'joiner' ] && ecmd="$ecmd -d"

    stagemsg="$stagemsg-XB-Encrypted"
}

get_socat_ver()
{
    [ -n "${SOCAT_VERSION+x}" ] && return
    # Determine the socat version
    SOCAT_VERSION=$(socat -V 2>&1 | \
                    grep -m1 -owE '[0-9]+(\.[0-9]+)+' | \
                    head -n1 || :)
    if [ -z "$SOCAT_VERSION" ]; then
        wsrep_log_error "******** FATAL ERROR ******************"
        wsrep_log_error "* Cannot determine the socat version. *"
        wsrep_log_error "***************************************"
        exit 2
    fi
}

get_transfer()
{
    if [ "$tfmt" = 'nc' ]; then
        wsrep_log_info "Using netcat as streamer"
        wsrep_check_programs nc
        tcmd='nc'
        if [ "$WSREP_SST_OPT_ROLE" = 'joiner' ]; then
            if nc -h 2>&1 | grep -q -F 'ncat'; then
                wsrep_log_info "Using Ncat as streamer"
                tcmd="$tcmd -l"
            elif nc -h 2>&1 | grep -qw -F -- '-d'; then
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
            if nc -h 2>&1 | grep -qw -F -- '-N'; then
                tcmd="$tcmd -N"
                wsrep_log_info "Using nc -N"
            fi
            # netcat doesn't understand [] around IPv6 address
            if nc -h 2>&1 | grep -q -F 'ncat'; then
                wsrep_log_info "Using Ncat as streamer"
            elif nc -h 2>&1 | grep -qw -F -- '-d'; then
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
            if [ "$sockopt" = "${sockopt#,pf=ip6,}" -a \
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
            local addr="$REMOTEIP:$SST_PORT"
            tcmd="socat -u stdio openssl-connect:$addr"
            action='Encrypting'
            get_socat_ver
            if ! check_for_version "$SOCAT_VERSION" '1.7.4.1'; then
                if check_for_version "$SOCAT_VERSION" '1.7.3.3'; then
                    # Workaround for a bug known as 'Red Hat issue 1870279'
                    # (connection reset by peer) in socat versions 1.7.3.3
                    # to 1.7.4.0:
                    tcmd="socat stdio openssl-connect:$addr,linger=10"
                    wsrep_log_info \
                        "Use workaround for socat $SOCAT_VERSION bug"
                fi
            fi
            if check_for_version "$SOCAT_VERSION" '1.7.4'; then
                tcmd="$tcmd,no-sni=1"
            fi
        fi

        if [ "${sockopt#*,dhparam=}" = "$sockopt" ]; then
            if [ -z "$ssl_dhparams" ]; then
                get_socat_ver
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
    cd "$DATA_DIR"
    local payload_data
    if [ "$OS" = 'Linux' ]; then
        payload_data=$(find $findopt . \
            -regex '.*undo[0-9]+$\|.*\.ibd$\|.*\.MYI$\|.*\.MYD$\|.*ibdata1$' \
            -type f -print0 | du --files0-from=- --bytes -c -s | \
            awk 'END { print $1 }')
    else
        payload_data=$(find $findopt . \
            -regex '.*undo[0-9]+$|.*\.ibd$|.*\.MYI$\.*\.MYD$|.*ibdata1$' \
            -type f -print0 | xargs -0 stat -f '%z' | \
            awk '{ sum += $1 } END { print sum }')
    fi
    local payload_undo=0
    if [ -n "$ib_undo_dir" -a "$ib_undo_dir" != '.' -a \
         "$ib_undo_dir" != "$DATA_DIR" -a -d "$ib_undo_dir" ]
    then
        cd "$ib_undo_dir"
        if [ "$OS" = 'Linux' ]; then
            payload_undo=$(find . -regex '.*undo[0-9]+$' -type f -print0 | \
                du --files0-from=- --bytes -c -s | awk 'END { print $1 }')
        else
            payload_undo=$(find . -regex '.*undo[0-9]+$' -type f -print0 | \
                xargs -0 stat -f '%z' | awk '{ sum += $1 } END { print sum }')
        fi
    fi
    cd "$OLD_PWD"

    wsrep_log_info \
        "SST footprint estimate: data: $payload_data, undo: $payload_undo"

    payload=$(( payload_data+payload_undo ))

    if [ "$compress" != 'none' ]; then
        # QuickLZ has around 50% compression ratio
        # When compression/compaction used, the progress is only an approximate.
        payload=$(( payload*1/2 ))
    fi

    if [ $WSREP_SST_OPT_PROGRESS -eq 1 ]; then
        # report to parent the total footprint of the SST
        echo "$TOTAL_TAG $payload"
    fi

    adjust_progress
}

adjust_progress()
{
    pcmd=""
    rcmd=""

    [ "$progress" = 'none' ] && return

    rlimitopts=""
    if [ -n "$rlimit" -a "$WSREP_SST_OPT_ROLE" = 'donor' ]; then
        wsrep_log_info "Rate-limiting SST to $rlimit"
        rlimitopts=" -L $rlimit"
    fi

    if [ -n "$progress" ]; then

        # Backward compatibility: user-configured progress output
        pcmd="pv $pvopts$rlimitopts"

        if [ -z "${PV_FORMAT+x}" ]; then
           PV_FORMAT=0
           pv --help | grep -qw -F -- '-F' && PV_FORMAT=1
        fi
        if [ $PV_FORMAT -eq 1 ]; then
            pcmd="$pcmd $pvformat"
        fi

        if [ $payload -ne 0 ]; then
            pcmd="$pcmd -s $payload"
        fi

        if [ "$progress" != '1' ]; then
            if [ -e "$progress" ]; then
                pcmd="$pcmd 2>>'$progress'"
            else
                pcmd="$pcmd 2>'$progress'"
            fi
        fi

    elif [ $WSREP_SST_OPT_PROGRESS -eq 1 ]; then

        # Default progress output parseable by parent
        pcmd="pv -f -i 1 -n -b$rlimitopts"

        # read progress data, add tag and post to stdout
        # for the parent
        rcmd="stdbuf -oL tr '\r' '\n' | xargs -n1 echo complete"

    elif [ -n "$rlimitopts" ]; then

        # Rate-limiting only, when rlimit is non-zero
        pcmd="pv -q$rlimitopts"

    fi
}

bkgroups='sst|xtrabackup|mariabackup'
encgroups="--mysqld|$bkgroups"

read_cnf()
{
    sfmt=$(parse_cnf sst streamfmt 'mbstream')
    tfmt=$(parse_cnf sst transferfmt 'socat')

    encrypt=$(parse_cnf "$encgroups" 'encrypt' 0)
    tmode=$(parse_cnf "$encgroups" 'ssl-mode' 'DISABLED' | \
            tr '[[:lower:]]' '[[:upper:]]')

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

    if [ $encrypt -ge 2 ]; then
        ssl_dhparams=$(parse_cnf "$encgroups" 'ssl-dhparams')
    fi

    sockopt=$(parse_cnf sst sockopt)
    progress=$(parse_cnf sst progress)
    ttime=$(parse_cnf sst time 0)
    cpat='.*\.pem$\|.*galera\.cache$\|.*sst_in_progress$\|.*\.sst$\|.*gvwstate\.dat$\|.*grastate\.dat$\|.*\.err$\|.*\.log$\|.*RPM_UPGRADE_MARKER$\|.*RPM_UPGRADE_HISTORY$'
    [ "$OS" = 'FreeBSD' ] && cpat=$(echo "$cpat" | sed 's/\\|/|/g')
    cpat=$(parse_cnf sst cpat "$cpat")
    scomp=$(parse_cnf sst compressor)
    sdecomp=$(parse_cnf sst decompressor)

    rlimit=$(parse_cnf sst rlimit)
    uextra=$(parse_cnf sst use-extra 0)
    speciald=$(parse_cnf sst 'sst-special-dirs' 1)
    iopts=$(parse_cnf "$bkgroups" 'inno-backup-opts')
    iapts=$(parse_cnf "$bkgroups" 'inno-apply-opts')
    impts=$(parse_cnf "$bkgroups" 'inno-move-opts')
    use_memory=$(parse_cnf "$bkgroups" 'use-memory')
    if [ -z "$use_memory" ]; then
        if [ -n "$INNODB_BUFFER_POOL_SIZE" ]; then
            use_memory="$INNODB_BUFFER_POOL_SIZE"
        else
            use_memory=$(parse_cnf '--mysqld' 'innodb-buffer-pool-size')
        fi
    fi
    stimeout=$(parse_cnf sst 'sst-initial-timeout' 300)
    ssyslog=$(parse_cnf sst 'sst-syslog' 0)
    ssystag=$(parse_cnf mysqld_safe 'syslog-tag' "${SST_SYSLOG_TAG:-}")
    ssystag="$ssystag-"
    sstlogarchive=$(parse_cnf sst 'sst-log-archive' 1)
    sstlogarchivedir=""
    if [ $sstlogarchive -ne 0 ]; then
        sstlogarchivedir=$(parse_cnf sst sst-log-archive-dir \
                           '/tmp/sst_log_archive')
        if [ -n "$sstlogarchivedir" ]; then
            sstlogarchivedir=$(trim_dir "$sstlogarchivedir")
        fi
    fi

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
            strmcmd="'$STREAM_BIN' -c '$INFO_FILE' '$DONOR_INFO_FILE'"
        fi
    else
        sfmt='tar'
        if [ "$WSREP_SST_OPT_ROLE" = 'joiner' ]; then
            strmcmd='tar xfi -'
        else
            strmcmd="tar cf - '$INFO_FILE' '$DONOR_INFO_FILE'"
        fi
    fi
    wsrep_log_info "Streaming with $sfmt"
}

cleanup_at_exit()
{
    # Since this is invoked just after exit NNN
    local estatus=$?
    if [ $estatus -ne 0 ]; then
        wsrep_log_error "Cleanup after exit with status: $estatus"
    elif [ -z "${coords:-}" -a "$WSREP_SST_OPT_ROLE" = 'joiner' ]; then
        estatus=32
        wsrep_log_error "Failed to get current position"
    fi

    [ "$(pwd)" != "$OLD_PWD" ] && cd "$OLD_PWD"

    if [ "$WSREP_SST_OPT_ROLE" = 'donor' -o $estatus -ne 0 ]; then
        if [ $estatus -ne 0 ]; then
            wsrep_log_error "Removing $MAGIC_FILE file due to signal"
        fi
        [ -f "$MAGIC_FILE" ] && rm -f "$MAGIC_FILE" || :
        [ -f "$DONOR_MAGIC_FILE" ] && rm -f "$DONOR_MAGIC_FILE" || :
        [ -f "$DATA/$IST_FILE" ] && rm -f "$DATA/$IST_FILE" || :
    fi

    if [ "$WSREP_SST_OPT_ROLE" = 'joiner' ]; then
        if [ -n "$BACKUP_PID" ]; then
            if ps -p $BACKUP_PID >/dev/null 2>&1; then
                wsrep_log_error \
                    "SST streaming process is still running. Killing..."
                cleanup_pid $BACKUP_PID
            fi
        fi
        wsrep_log_info "Removing the sst_in_progress file"
        wsrep_cleanup_progress_file
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
    local pgid=$(ps -o 'pgid=' $$ 2>/dev/null | grep -o -E '[0-9]+' || :)

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

    if [ -n "$SST_PID" ]; then
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
        if check_port "" "$SST_PORT" 'socat|nc|netcat'; then
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
                # mariadb-backup works only locally.
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
            local koption=0
            if [ "$OS" = 'FreeBSD' -o "$OS" = 'NetBSD' -o "$OS" = 'OpenBSD' -o \
                 "$OS" = 'DragonFly' ]; then
                if timeout 2>&1 | grep -qw -F -- '-k'; then
                    koption=1
                fi
            else
                if timeout --help | grep -qw -F -- '-k'; then
                    koption=1
                fi
            fi
            if [ $koption -ne 0 ]; then
                ltcmd="timeout -k $(( tmt+10 )) $tmt $tcmd"
            else
                ltcmd="timeout -s 9 $tmt $tcmd"
            fi
        fi
    fi

    if [ $wait -ne 0 ]; then
        wait_for_listen &
    fi

    cd "$dir"
    set +e
    timeit "$msg" "$ltcmd | $strmcmd; RC=( "\${PIPESTATUS[@]}" )"
    set -e
    cd "$OLD_PWD"

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
        if [ -r "$MAGIC_FILE" ]; then
            :
        elif [ -r "$dir/xtrabackup_galera_info" ]; then
            mv "$dir/xtrabackup_galera_info" "$MAGIC_FILE"
            wsrep_log_info "the SST donor uses an old version" \
                           "of mariabackup or xtrabackup"
        else
            # this message should cause joiner to abort:
            wsrep_log_error "receiving process ended without creating" \
                            "magic file ($MAGIC_FILE)"
            wsrep_log_info "Contents of datadir:"
            wsrep_log_info "$(ls -l "$dir"/*)"
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

        if [ $WSREP_SST_OPT_PROGRESS -eq 1 ]; then
            # check total SST footprint
            payload=$(grep -m1 -E "^$TOTAL_TAG[[:space:]]" "$MAGIC_FILE" || :)
            if [ -n "$payload" ]; then
                payload=$(trim_string "${payload#$TOTAL_TAG}")
                if [ $payload -ge 0 ]; then
                    # report to parent
                    echo "$TOTAL_TAG $payload"
                fi
            fi
        fi
    fi
}

send_donor()
{
    local dir="$1"
    local msg="$2"

    cd "$dir"
    set +e
    timeit "$msg" "$strmcmd | $tcmd; RC=( "\${PIPESTATUS[@]}" )"
    set -e
    cd "$OLD_PWD"

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

    while :; do
        if ! ps -p $WSREP_SST_OPT_PARENT >/dev/null 2>&1; then
            wsrep_log_error \
                "Parent mysqld process (PID: $WSREP_SST_OPT_PARENT)" \
                "terminated unexpectedly."
            kill -- -$WSREP_SST_OPT_PARENT
            exit 32
        fi
        if ! ps -p $sst_stream_pid >/dev/null 2>&1; then
            break
        fi
        sleep 0.1
    done
}

read_cnf
setup_ports

if [ "$progress" = 'none' ]; then
    wsrep_log_info "All progress/rate-limiting disabled by configuration"
elif [ -z "$(commandex pv)" ]; then
    wsrep_log_info "Progress reporting tool pv not found in path: $PATH"
    wsrep_log_info "Disabling all progress/rate-limiting"
    progress='none'
fi

if "$BACKUP_BIN" --help 2>/dev/null | grep -qw -F -- '--version-check'; then
    disver=' --no-version-check'
fi

get_stream
get_transfer

findopt='-L'
[ "$OS" = 'FreeBSD' ] && findopt="$findopt -E"

wait_previous_sst

[ -f "$MAGIC_FILE" ] && rm -f "$MAGIC_FILE"
[ -f "$DONOR_MAGIC_FILE" ] && rm -f "$DONOR_MAGIC_FILE"
[ -f "$DATA/$IST_FILE" ] && rm -f "$DATA/$IST_FILE"

if [ $ssyslog -eq 1 ]; then
    if [ -n "$(commandex logger)" ]; then
        wsrep_log_info "Logging all stderr of SST/mariadb-backup to syslog"

        exec 2> >(logger -p daemon.err -t ${ssystag}wsrep-sst-$WSREP_SST_OPT_ROLE)

        wsrep_log_error()
        {
            logger -p daemon.err -t ${ssystag}wsrep-sst-$WSREP_SST_OPT_ROLE -- "$@"
        }

        wsrep_log_warning()
        {
            logger -p daemon.warning -t ${ssystag}wsrep-sst-$WSREP_SST_OPT_ROLE -- "$@"
        }

        wsrep_log_info()
        {
            logger -p daemon.info -t ${ssystag}wsrep-sst-$WSREP_SST_OPT_ROLE -- "$@"
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
        ARCHIVETIMESTAMP=$(date '+%Y.%m.%d-%H.%M.%S.%N')

        if [ -n "$sstlogarchivedir" ]; then
            if [ ! -d "$sstlogarchivedir" ]; then
                if ! mkdir -p "$sstlogarchivedir"; then
                    sstlogarchivedir=""
                    wsrep_log_warning \
                        "Unable to create '$sstlogarchivedir' directory"
                fi
            elif [ ! -w "$sstlogarchivedir" ]; then
                sstlogarchivedir=""
                wsrep_log_warning \
                    "The '$sstlogarchivedir' directory is not writtable"
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
            mv "$INNOAPPLYLOG" "$newfile" && gzip "$newfile" || \
                wsrep_log_warning "Failed to archive log file ('$newfile')"
        fi

        if [ -e "$INNOMOVELOG" ]; then
            if [ -n "$sstlogarchivedir" ]; then
                newfile=$(basename "$INNOMOVELOG")
                newfile="$sstlogarchivedir/$newfile.$ARCHIVETIMESTAMP"
            else
                newfile="$INNOMOVELOG.$ARCHIVETIMESTAMP"
            fi
            wsrep_log_info "Moving '$INNOMOVELOG' to '$newfile'"
            mv "$INNOMOVELOG" "$newfile" && gzip "$newfile" || \
                wsrep_log_warning "Failed to archive log file ('$newfile')"
        fi

        if [ -e "$INNOBACKUPLOG" ]; then
            if [ -n "$sstlogarchivedir" ]; then
                newfile=$(basename "$INNOBACKUPLOG")
                newfile="$sstlogarchivedir/$newfile.$ARCHIVETIMESTAMP"
            else
                newfile="$INNOBACKUPLOG.$ARCHIVETIMESTAMP"
            fi
            wsrep_log_info "Moving '$INNOBACKUPLOG' to '$newfile'"
            mv "$INNOBACKUPLOG" "$newfile" && gzip "$newfile" || \
                wsrep_log_warning "Failed to archive log file ('$newfile')"
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
    if [ -n "$use_memory" ]; then
        INNOEXTRA="$INNOEXTRA --use-memory=$use_memory"
    fi
    INNOAPPLY="$BACKUP_BIN --prepare$disver$recovery${iapts:+ }$iapts$INNOEXTRA --target-dir='$DATA' --datadir='$DATA'$mysqld_args $INNOAPPLY"
    INNOMOVE="$BACKUP_BIN$WSREP_SST_OPT_CONF --move-back$disver${impts:+ }$impts$INNOEXTRA --galera-info --force-non-empty-directories --target-dir='$DATA' --datadir='${TDATA:-$DATA}' $INNOMOVE"
    INNOBACKUP="$BACKUP_BIN$WSREP_SST_OPT_CONF --backup$disver${iopts:+ }$iopts$tmpopts$INNOEXTRA --galera-info --stream=$sfmt --target-dir='$itmpdir' --datadir='$DATA'$mysqld_args $INNOBACKUP"
}

send_magic()
{
    # Store donor's wsrep GTID (state ID) and wsrep_gtid_domain_id
    # (separated by a space).
    echo "$WSREP_SST_OPT_GTID $WSREP_SST_OPT_GTID_DOMAIN_ID" > "$MAGIC_FILE"
    echo "$WSREP_SST_OPT_GTID $WSREP_SST_OPT_GTID_DOMAIN_ID" > "$DONOR_MAGIC_FILE"
    if [ -n "$WSREP_SST_OPT_REMOTE_PSWD" ]; then
        # Let joiner know that we know its secret
        echo "$SECRET_TAG $WSREP_SST_OPT_REMOTE_PSWD" >> "$MAGIC_FILE"
    fi

    if [ $WSREP_SST_OPT_BYPASS -eq 0 -a $WSREP_SST_OPT_PROGRESS -eq 1 ]; then
        # Tell joiner what to expect:
        echo "$TOTAL_TAG $payload" >> "$MAGIC_FILE"
    fi
}

if [ "$WSREP_SST_OPT_ROLE" = 'donor' ]; then

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
            itmpdir="$(mktemp -d)"
        elif [ "$OS" = 'Linux' ]; then
            xtmpdir=$(mktemp -d "--tmpdir=$tmpdir")
            itmpdir=$(mktemp -d "--tmpdir=$tmpdir")
        else
            xtmpdir=$(TMPDIR="$tmpdir"; mktemp -d)
            itmpdir=$(TMPDIR="$tmpdir"; mktemp -d)
        fi

        wsrep_log_info "Using '$xtmpdir' as mariadb-backup temporary directory"
        tmpopts=" --tmpdir='$xtmpdir'"

        wsrep_log_info "Using '$itmpdir' as mariadb-backup working directory"

        if [ -n "$WSREP_SST_OPT_USER" ]; then
           INNOEXTRA="$INNOEXTRA --user='$WSREP_SST_OPT_USER'"
        fi

        if [ -n "$WSREP_SST_OPT_PSWD" ]; then
            export MYSQL_PWD="$WSREP_SST_OPT_PSWD"
        elif [ -n "$WSREP_SST_OPT_USER" ]; then
            # Empty password, used for testing, debugging etc.
            unset MYSQL_PWD
        fi

        check_extra

        if [ -n "$progress" -o $WSREP_SST_OPT_PROGRESS -eq 1 ]; then
            wsrep_log_info "Estimating total transfer size"
            get_footprint
            wsrep_log_info "To transfer: $payload"
        else
            adjust_progress
        fi

        wsrep_log_info "Streaming GTID file before SST"
        send_magic

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

        if [ -n "$pcmd" ]; then
            if [ -n "$rcmd" ]; then
                # redirect pv stderr to rcmd for tagging and output to parent
                tcmd="{ $pcmd 2>&3 | $tcmd; } 3>&1 | $rcmd"
            else
                # use user-configured pv output
                tcmd="$pcmd | $tcmd"
            fi
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

        # if compression is enabled for backup files, then add the
        # appropriate options to the mariadb-backup command line:
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
            wsrep_log_error "mariadb-backup finished with error: ${RC[0]}." \
                            "Check syslog or '$INNOBACKUPLOG' for details"
            exit 22
        elif [ ${RC[$(( ${#RC[@]}-1 ))]} -eq 1 ]; then
            wsrep_log_error "$tcmd finished with error: ${RC[1]}"
            exit 22
        fi

    else # BYPASS FOR IST

        wsrep_log_info "Bypassing the SST for IST"
        echo 'continue' # now server can resume updating data

        send_magic

        echo '1' > "$DATA/$IST_FILE"

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

else # joiner

    create_dirs 1

    [ -e "$SST_PROGRESS_FILE" ] && \
        wsrep_log_info "Stale sst_in_progress file: $SST_PROGRESS_FILE"
    [ -n "$SST_PROGRESS_FILE" ] && touch "$SST_PROGRESS_FILE"

    if [ -n "$backup_threads" ]; then
        impts="--parallel=$backup_threads${impts:+ }$impts"
    fi

    stagemsg='Joiner-Recv'

    MODULE="${WSREP_SST_OPT_MODULE:-xtrabackup_sst}"
    ADDR="$WSREP_SST_OPT_HOST"

    if [ "${tmode#VERIFY}" != "$tmode" ]; then
        # backward-incompatible behavior:
        CN=""
        if [ -n "$tpem" ]; then
            CN=$(openssl_getCN "$tpem")
        fi
        MY_SECRET="$(wsrep_gen_secret)"
        # Add authentication data to address
        ADDR="$CN:$MY_SECRET@$ADDR"
    else
        MY_SECRET="" # for check down in recv_joiner()
    fi

    get_keys
    if [ $encrypt -eq 1 ]; then
        strmcmd="$ecmd | $strmcmd"
    fi

    if [ -n "$sdecomp" ]; then
        strmcmd="$sdecomp | $strmcmd"
    fi

    check_sockets_utils

    trap cleanup_at_exit EXIT

    STATDIR="$(mktemp -d)"
    MAGIC_FILE="$STATDIR/$INFO_FILE"
    DONOR_MAGIC_FILE="$STATDIR/$DONOR_INFO_FILE"

    recv_joiner "$STATDIR" "$stagemsg-gtid" $stimeout 1 1

    if ! ps -p "$WSREP_SST_OPT_PARENT" >/dev/null 2>&1; then
        wsrep_log_error "Parent mysqld process (PID: $WSREP_SST_OPT_PARENT)" \
                        "terminated unexpectedly."
        exit 32
    fi

    if [ ! -r "$STATDIR/$IST_FILE" ]; then

        adjust_progress
        if [ -n "$pcmd" ]; then
            if [ -n "$rcmd" ]; then
                # redirect pv stderr to rcmd for tagging and output to parent
                strmcmd="{ $pcmd 2>&3 | $strmcmd; } 3>&1 | $rcmd"
            else
                # use user-configured pv output
                strmcmd="$pcmd | $strmcmd"
            fi
        fi

        if [ -d "$DATA/.sst" ]; then
            wsrep_log_info \
                "WARNING: Stale temporary SST directory:" \
                "'$DATA/.sst' from previous state transfer, removing..."
            rm -rf "$DATA/.sst"
        fi
        mkdir -p "$DATA/.sst"
        (recv_joiner "$DATA/.sst" "$stagemsg-SST" 0 0 0) &
        BACKUP_PID=$!
        wsrep_log_info "Proceeding with SST"

        get_binlog

        if [ -n "$WSREP_SST_OPT_BINLOG" ]; then
            binlog_dir=$(dirname "$WSREP_SST_OPT_BINLOG")
            binlog_base=$(basename "$WSREP_SST_OPT_BINLOG")
            binlog_index="$WSREP_SST_OPT_BINLOG_INDEX"
            cd "$DATA"
            wsrep_log_info "Cleaning the old binary logs"
            # If there is a file with binlogs state, delete it:
            [ -f "$binlog_base.state" ] && rm "$binlog_base.state" >&2
            # Clean up the old binlog files and index:
            if [ -f "$binlog_index" ]; then
                while read bin_file || [ -n "$bin_file" ]; do
                    rm -f "$bin_file" >&2 || :
                done < "$binlog_index"
                rm "$binlog_index" >&2
            fi
            if [ -n "$binlog_dir" -a "$binlog_dir" != '.' -a \
                 -d "$binlog_dir" ]
            then
                cd "$binlog_dir"
                if [ "$(pwd)" != "$DATA_DIR" ]; then
                    wsrep_log_info \
                       "Cleaning the binlog directory '$binlog_dir' as well"
                fi
            fi
            rm -f "$binlog_base".[0-9]* >&2 || :
            cd "$OLD_PWD"
        fi

        wsrep_log_info \
            "Cleaning the existing datadir and innodb-data/log directories"

        find $findopt ${ib_home_dir:+"$ib_home_dir"} \
                ${ib_undo_dir:+"$ib_undo_dir"} \
                ${ib_log_dir:+"$ib_log_dir"} \
                ${ar_log_dir:+"$ar_log_dir"} \
                "$DATA" -mindepth 1 -prune -regex "$cpat" \
                -o -exec rm -rf {} >&2 \+

        # Deleting legacy files from old versions:
        [ -f "$DATA/xtrabackup_binary" ]      && rm -f "$DATA/xtrabackup_binary"
        [ -f "$DATA/xtrabackup_pid" ]         && rm -f "$DATA/xtrabackup_pid"
        [ -f "$DATA/xtrabackup_checkpoints" ] && rm -f "$DATA/xtrabackup_checkpoints"
        [ -f "$DATA/xtrabackup_info" ]        && rm -f "$DATA/xtrabackup_info"
        [ -f "$DATA/xtrabackup_slave_info" ]  && rm -f "$DATA/xtrabackup_slave_info"
        [ -f "$DATA/xtrabackup_binlog_info" ] && rm -f "$DATA/xtrabackup_binlog_info"
        [ -f "$DATA/xtrabackup_binlog_pos_innodb" ] && rm -f "$DATA/xtrabackup_binlog_pos_innodb"

        # Deleting files from previous SST:
        [ -f "$DATA/mariadb_backup_checkpoints" ] && rm -f "$DATA/mariadb_backup_checkpoints"
        [ -f "$DATA/mariadb_backup_info" ]        && rm -f "$DATA/mariadb_backup_info"
        [ -f "$DATA/mariadb_backup_slave_info" ]  && rm -f "$DATA/mariadb_backup_slave_info"
        [ -f "$DATA/mariadb_backup_binlog_info" ] && rm -f "$DATA/mariadb_backup_binlog_info"
        [ -f "$DATA/mariadb_backup_binlog_pos_innodb" ] && rm -f "$DATA/mariadb_backup_binlog_pos_innodb"

        TDATA="$DATA"
        DATA="$DATA/.sst"
        MAGIC_FILE="$DATA/$INFO_FILE"

        wsrep_log_info "Waiting for SST streaming to complete!"
        monitor_process $BACKUP_PID
        BACKUP_PID=""

        # It is possible that the old version of the galera
        # information file will be transferred second time:
        if [ ! -f "$DATA/$INFO_FILE" -a \
               -f "$DATA/xtrabackup_galera_info" ]
        then
            mv "$DATA/xtrabackup_galera_info" "$DATA/$INFO_FILE"
        fi

        # Correcting the name of the common information file
        # if the donor has an old version:
        if [ ! -f "$DATA/mariadb_backup_info" -a \
               -f "$DATA/xtrabackup_info" ]
        then
            mv "$DATA/xtrabackup_info" "$DATA/mariadb_backup_info"
            wsrep_log_info "general information file with a legacy" \
                           "name has been renamed"
        fi

        # Correcting the name for the file with the binlog position
        # for the master if the donor has an old version:
        if [ ! -f "$DATA/mariadb_backup_slave_info" -a \
               -f "$DATA/xtrabackup_slave_info" ]
        then
            mv "$DATA/xtrabackup_slave_info" "$DATA/mariadb_backup_slave_info"
            wsrep_log_info "binlog position file with a legacy" \
                           "name has been renamed"
        fi

        # An old version of the donor may send a checkpoints
        # list file under an outdated name:
        if [ ! -f "$DATA/mariadb_backup_checkpoints" -a \
               -f "$DATA/xtrabackup_checkpoints" ]
        then
            mv "$DATA/xtrabackup_checkpoints" "$DATA/mariadb_backup_checkpoints"
            wsrep_log_info "list of checkpoints with a legacy" \
                           "name has been renamed"
        fi

        if [ ! -s "$DATA/mariadb_backup_checkpoints" ]; then
            wsrep_log_error "mariadb_backup_checkpoints missing," \
                            "failed mariadb-backup/SST on donor"
            exit 2
        fi

        # Compact backups are not supported by mariadb-backup
        if grep -qw -F 'compact = 1' "$DATA/mariadb_backup_checkpoints"; then
            wsrep_log_info "Index compaction detected"
            wsrep_log_error "Compact backups are not supported by mariadb-backup"
            exit 2
        fi

        qpfiles=$(find $findopt "$DATA" -maxdepth 1 -type f -name '*.qp' -print -quit)
        if [ -n "$qpfiles" ]; then
            wsrep_log_info "Compressed qpress files found"

            if [ -z "$(commandex qpress)" ]; then
                wsrep_log_error "qpress utility not found in the path"
                exit 22
            fi

            get_proc

            dcmd="xargs -n 2 qpress -dT$nproc"

            if [ -n "$progress" -a "$progress" != 'none' ] && \
               pv --help | grep -qw -F -- '--line-mode'
            then
                count=$(find $findopt "$DATA" -maxdepth 1 -type f -name '*.qp' | wc -l)
                count=$(( count*2 ))
                pvopts='-f -l -N Decompression'
                pvformat="-F '%N => Rate:%r Elapsed:%t %e Progress: [%b/$count]'"
                payload=$count
                adjust_progress
                dcmd="$pcmd | $dcmd"
            fi

            # Decompress the qpress files
            wsrep_log_info "Decompression with $nproc threads"
            timeit 'Joiner-Decompression' \
                   "find $findopt '$DATA' -type f -name '*.qp' -printf '%p\n%h\n' | \
                   $dcmd"
            extcode=$?

            if [ $extcode -eq 0 ]; then
                wsrep_log_info "Removing qpress files after decompression"
                find $findopt "$DATA" -type f -name '*.qp' -delete
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

        # An old version of the donor may send a binary logs
        # list file under an outdated name:
        if [ ! -f "$DATA/mariadb_backup_binlog_info" -a \
               -f "$DATA/xtrabackup_binlog_info" ]
        then
            mv "$DATA/xtrabackup_binlog_info" "$DATA/mariadb_backup_binlog_info"
            wsrep_log_info "list of binary logs with a legacy" \
                           "name has been renamed"
        fi

        wsrep_log_info "Preparing the backup at $DATA"
        setup_commands
        timeit 'mariadb-backup prepare stage' "$INNOAPPLY"
        if [ $? -ne 0 ]; then
            wsrep_log_error "mariadb-backup apply finished with errors." \
                            "Check syslog or '$INNOAPPLYLOG' for details."
            exit 22
        fi

        if [ -n "$WSREP_SST_OPT_BINLOG" ]; then
            cd "$DATA"
            binlogs=""
            if [ -f 'mariadb_backup_binlog_info' ]; then
                NL=$'\n'
                while read bin_string || [ -n "$bin_string" ]; do
                    bin_file=$(echo "$bin_string" | cut -f1)
                    if [ -f "$bin_file" ]; then
                        binlogs="$binlogs${binlogs:+$NL}$bin_file"
                    fi
                done < 'mariadb_backup_binlog_info'
            else
                binlogs=$(ls -d -1 "$binlog_base".[0-9]* 2>/dev/null || :)
            fi
            cd "$DATA_DIR"
            if [ -n "$binlog_dir" -a "$binlog_dir" != '.' -a \
                 "$binlog_dir" != "$DATA_DIR" ]
            then
                [ ! -d "$binlog_dir" ] && mkdir -p "$binlog_dir"
            fi
            index_dir=$(dirname "$binlog_index");
            if [ -n "$index_dir" -a "$index_dir" != '.' -a \
                 "$index_dir" != "$DATA_DIR" ]
            then
                [ ! -d "$index_dir" ] && mkdir -p "$index_dir"
            fi
            if [ -n "$binlogs" ]; then
                wsrep_log_info "Moving binary logs to $binlog_dir"
                echo "$binlogs" | \
                while read bin_file || [ -n "$bin_file" ]; do
                    mv "$DATA/$bin_file" "$binlog_dir"
                    echo "$binlog_dir${binlog_dir:+/}$bin_file" >> "$binlog_index"
                done
            fi
            cd "$OLD_PWD"
        fi

        MAGIC_FILE="$TDATA/$INFO_FILE"
        DONOR_MAGIC_FILE="$TDATA/$DONOR_INFO_FILE"

        wsrep_log_info "Moving the backup to $TDATA"
        timeit 'mariadb-backup move stage' "$INNOMOVE"
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
        if [ $WSREP_SST_OPT_BYPASS -eq 0 ]; then
            readonly WSREP_SST_OPT_BYPASS=1
            readonly WSREP_TRANSFER_TYPE='IST'
        fi

    fi

    if [ ! -r "$MAGIC_FILE" ]; then
        wsrep_log_error "Internal error: SST magic file '$MAGIC_FILE'" \
                        "not found or not readable"
        exit 2
    fi

    simulate_long_sst

    # use donor magic file, if present
    # if IST was used, donor magic file was not created
    # Remove special tags from the magic file, and from the output:
    if [ -r "$DONOR_MAGIC_FILE" ]; then
        coords=$(head -n1 "$DONOR_MAGIC_FILE")
        wsrep_log_info "Galera co-ords from donor: $coords"
    else
        coords=$(head -n1 "$MAGIC_FILE")
        wsrep_log_info "Galera co-ords from recovery: $coords"
    fi
    echo "$coords" # Output : UUID:seqno wsrep_gtid_domain_id

    wsrep_log_info "Total time on joiner: $totime seconds"
fi

wsrep_log_info "$WSREP_METHOD $WSREP_TRANSFER_TYPE completed on $WSREP_SST_OPT_ROLE"
exit 0
