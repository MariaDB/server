# Copyright (C) 2012-2015 Codership Oy
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

# This is a common command line parser to be sourced by other SST scripts

set -u

WSREP_SST_OPT_BYPASS=0
WSREP_SST_OPT_BINLOG=""
WSREP_SST_OPT_BINLOG_INDEX=""
WSREP_SST_OPT_DATA=""
WSREP_SST_OPT_AUTH=${WSREP_SST_OPT_AUTH:-}
WSREP_SST_OPT_USER=${WSREP_SST_OPT_USER:-}
WSREP_SST_OPT_PSWD=${WSREP_SST_OPT_PSWD:-}
WSREP_SST_OPT_DEFAULT=""
WSREP_SST_OPT_EXTRA_DEFAULT=""
WSREP_SST_OPT_SUFFIX_DEFAULT=""
WSREP_SST_OPT_SUFFIX_VALUE=""
INNODB_DATA_HOME_DIR_ARG=""

while [ $# -gt 0 ]; do
case "$1" in
    '--address')
        readonly WSREP_SST_OPT_ADDR="$2"
        #
        # Break address string into host:port/path parts
        #
        case "${WSREP_SST_OPT_ADDR}" in
        \[*)
            # IPv6
            addr_no_bracket=${WSREP_SST_OPT_ADDR#\[}
            readonly WSREP_SST_OPT_HOST_UNESCAPED=${addr_no_bracket%%\]*}
            readonly WSREP_SST_OPT_HOST="[${WSREP_SST_OPT_HOST_UNESCAPED}]"
            readonly WSREP_SST_OPT_HOST_ESCAPED="\\[${WSREP_SST_OPT_HOST_UNESCAPED}\\]"
            ;;
        *)
            readonly WSREP_SST_OPT_HOST=${WSREP_SST_OPT_ADDR%%[:/]*}
            readonly WSREP_SST_OPT_HOST_UNESCAPED=$WSREP_SST_OPT_HOST
            readonly WSREP_SST_OPT_HOST_ESCAPED=$WSREP_SST_OPT_HOST
            ;;
        esac
        remain=${WSREP_SST_OPT_ADDR#${WSREP_SST_OPT_HOST_ESCAPED}}
        remain=${remain#:}
        readonly WSREP_SST_OPT_ADDR_PORT=${remain%%/*}
        remain=${remain#*/}
        readonly WSREP_SST_OPT_MODULE=${remain%%/*}
        readonly WSREP_SST_OPT_PATH=${WSREP_SST_OPT_ADDR#*/}
        remain=${WSREP_SST_OPT_PATH#*/}
        if [ "$remain" != "${WSREP_SST_OPT_PATH}" ]; then
            readonly WSREP_SST_OPT_LSN=${remain%%/*}
            remain=${remain#*/}
            if [ "$remain" != "${WSREP_SST_OPT_LSN}" ]; then
                readonly WSREP_SST_OPT_SST_VER=${remain%%/*}
            else
                readonly WSREP_SST_OPT_SST_VER=""
            fi
        else
            readonly WSREP_SST_OPT_LSN=""
            readonly WSREP_SST_OPT_SST_VER=""
        fi
        shift
        ;;
    '--bypass')
        WSREP_SST_OPT_BYPASS=1
        ;;
    '--datadir')
        readonly WSREP_SST_OPT_DATA="$2"
        shift
        ;;
    '--innodb-data-home-dir')
        readonly INNODB_DATA_HOME_DIR_ARG="$2"
        shift
        ;;
    '--defaults-file')
        readonly WSREP_SST_OPT_DEFAULT="$1=$2"
        shift
        ;;
    '--defaults-extra-file')
        readonly WSREP_SST_OPT_EXTRA_DEFAULT="$1=$2"
        shift
        ;;
    '--defaults-group-suffix')
        readonly WSREP_SST_OPT_SUFFIX_DEFAULT="$1=$2"
        readonly WSREP_SST_OPT_SUFFIX_VALUE="$2"
        shift
        ;;
    '--host')
        readonly WSREP_SST_OPT_HOST="$2"
        shift
        ;;
    '--local-port')
        readonly WSREP_SST_OPT_LPORT="$2"
        shift
        ;;
    '--parent')
        readonly WSREP_SST_OPT_PARENT="$2"
        shift
        ;;
    '--password')
        WSREP_SST_OPT_PSWD="$2"
        shift
        ;;
    '--port')
        readonly WSREP_SST_OPT_PORT="$2"
        shift
        ;;
    '--role')
        readonly WSREP_SST_OPT_ROLE="$2"
        shift
        ;;
    '--socket')
        readonly WSREP_SST_OPT_SOCKET="$2"
        shift
        ;;
    '--user')
        WSREP_SST_OPT_USER="$2"
        shift
        ;;
    '--gtid')
        readonly WSREP_SST_OPT_GTID="$2"
        shift
        ;;
    '--binlog')
        WSREP_SST_OPT_BINLOG="$2"
        shift
        ;;
    '--binlog-index')
	WSREP_SST_OPT_BINLOG_INDEX="$2"
	shift
	;;
    '--gtid-domain-id')
        readonly WSREP_SST_OPT_GTID_DOMAIN_ID="$2"
        shift
        ;;
    *) # must be command
       # usage
       # exit 1
       ;;
esac
shift
done
readonly WSREP_SST_OPT_BYPASS
readonly WSREP_SST_OPT_BINLOG
readonly WSREP_SST_OPT_BINLOG_INDEX

if [ -n "${WSREP_SST_OPT_ADDR_PORT:-}" ]; then
  if [ -n "${WSREP_SST_OPT_PORT:-}" ]; then
    if [ "$WSREP_SST_OPT_PORT" != "$WSREP_SST_OPT_ADDR_PORT" ]; then
      echo "WSREP_SST: [ERROR] port in --port=$WSREP_SST_OPT_PORT differs from port in --address=$WSREP_SST_OPT_ADDR" >&2
      exit 2
    fi
  else
    readonly WSREP_SST_OPT_PORT="$WSREP_SST_OPT_ADDR_PORT"
  fi
fi

# try to use my_print_defaults, mysql and mysqldump that come with the sources
# (for MTR suite)
SCRIPTS_DIR="$(cd $(dirname "$0"); pwd -P)"
EXTRA_DIR="$SCRIPTS_DIR/../extra"
CLIENT_DIR="$SCRIPTS_DIR/../client"

if [ -x "$CLIENT_DIR/mysql" ]; then
    MYSQL_CLIENT="$CLIENT_DIR/mysql"
else
    MYSQL_CLIENT=mysql
fi

if [ -x "$CLIENT_DIR/mysqldump" ]; then
    MYSQLDUMP="$CLIENT_DIR/mysqldump"
else
    MYSQLDUMP=mysqldump
fi

if [ -x "$SCRIPTS_DIR/my_print_defaults" ]; then
    MY_PRINT_DEFAULTS="$SCRIPTS_DIR/my_print_defaults"
elif [ -x "$EXTRA_DIR/my_print_defaults" ]; then
    MY_PRINT_DEFAULTS="$EXTRA_DIR/my_print_defaults"
else
    MY_PRINT_DEFAULTS=my_print_defaults
fi

readonly WSREP_SST_OPT_CONF="$WSREP_SST_OPT_DEFAULT $WSREP_SST_OPT_EXTRA_DEFAULT $WSREP_SST_OPT_SUFFIX_DEFAULT"
readonly MY_PRINT_DEFAULTS="$MY_PRINT_DEFAULTS $WSREP_SST_OPT_CONF"

wsrep_auth_not_set()
{
    [ -z "$WSREP_SST_OPT_AUTH" -o "$WSREP_SST_OPT_AUTH" = "(null)" ]
}

# State Snapshot Transfer authentication password was displayed in the ps output. Bug fixed #1200727.
if $MY_PRINT_DEFAULTS sst | grep -q "wsrep_sst_auth"; then
    if wsrep_auth_not_set; then
            WSREP_SST_OPT_AUTH=$($MY_PRINT_DEFAULTS sst | grep -- "--wsrep_sst_auth" | cut -d= -f2)
    fi
fi
readonly WSREP_SST_OPT_AUTH

# Splitting AUTH into potential user:password pair
if ! wsrep_auth_not_set
then
    WSREP_SST_OPT_USER="${WSREP_SST_OPT_AUTH%%:*}"
    WSREP_SST_OPT_PSWD="${WSREP_SST_OPT_AUTH##*:}"
fi
readonly WSREP_SST_OPT_USER
readonly WSREP_SST_OPT_PSWD

if [ -n "${WSREP_SST_OPT_DATA:-}" ]
then
    SST_PROGRESS_FILE="$WSREP_SST_OPT_DATA/sst_in_progress"
else
    SST_PROGRESS_FILE=""
fi

wsrep_log()
{
    # echo everything to stderr so that it gets into common error log
    # deliberately made to look different from the rest of the log
    local readonly tst="$(date +%Y%m%d\ %H:%M:%S.%N | cut -b -21)"
    echo "WSREP_SST: $* ($tst)" >&2
}

wsrep_log_error()
{
    wsrep_log "[ERROR] $*"
}

wsrep_log_warning()
{
    wsrep_log "[WARNING] $*"
}

wsrep_log_info()
{
    wsrep_log "[INFO] $*"
}

wsrep_cleanup_progress_file()
{
    [ -n "${SST_PROGRESS_FILE:-}" ] && rm -f "$SST_PROGRESS_FILE" 2>/dev/null || true
}

wsrep_check_program()
{
    local prog=$1

    if ! command -v $prog >/dev/null
    then
        echo "'$prog' not found in PATH"
        exit 2 # ENOENT no such file or directory
    fi
}

wsrep_check_programs()
{
    local ret=0

    while [ $# -gt 0 ]
    do
        wsrep_check_program $1
        shift
    done
}

#
# user can specify mariabackup specific settings that will be used during sst
# process like encryption, etc.....
# parse such configuration option. (group for xb settings is [sst] in my.cnf
#
# 1st param: group (config file section like sst) or my_print_defaults argument (like --mysqld)
# 2nd param: var : name of the variable in the section, e.g. server-id
# 3rd param: - : default value for the param
parse_cnf()
{
    local group=$1
    local var=$2
    local reval=""

    # normalize the variable names specified in cnf file (user can use _ or - for example log-bin or log_bin)
    # then search for needed variable
    # finally get the variable value (if variables has been specified multiple time use the last value only)

    reval=$($MY_PRINT_DEFAULTS "${group}" | awk -v var="${var}" 'BEGIN { OFS=FS="=" } { gsub(/_/,"-",$1); if ( $1=="--"var) lastval=substr($0,length($1)+2) } END { print lastval}')

    # use default if we haven't found a value
    if [ -z "$reval" ]; then
        [ -n "$3" ] && reval=$3
    fi
    echo $reval
}
