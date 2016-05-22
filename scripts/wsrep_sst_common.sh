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
WSREP_SST_OPT_CONF_SUFFIX=""
WSREP_SST_OPT_DATA=""
WSREP_SST_OPT_AUTH=${WSREP_SST_OPT_AUTH:-}
WSREP_SST_OPT_USER=${WSREP_SST_OPT_USER:-}
WSREP_SST_OPT_PSWD=${WSREP_SST_OPT_PSWD:-}
WSREP_SST_OPT_DEFAULT=""
WSREP_SST_OPT_EXTRA_DEFAULT=""

while [ $# -gt 0 ]; do
case "$1" in
    '--address')
        readonly WSREP_SST_OPT_ADDR="$2"
        shift
        ;;
    '--bypass')
        WSREP_SST_OPT_BYPASS=1
        ;;
    '--datadir')
        readonly WSREP_SST_OPT_DATA="$2"
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
        WSREP_SST_OPT_CONF_SUFFIX="$2"
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
readonly WSREP_SST_OPT_CONF_SUFFIX

# try to use my_print_defaults, mysql and mysqldump that come with the sources
# (for MTR suite)
SCRIPTS_DIR="$(cd $(dirname "$0"); pwd -P)"
EXTRA_DIR="$SCRIPTS_DIR/../extra"
CLIENT_DIR="$SCRIPTS_DIR/../client"

if [ -x "$CLIENT_DIR/mysql" ]; then
    MYSQL_CLIENT="$CLIENT_DIR/mysql"
else
    MYSQL_CLIENT=$(which mysql)
fi

if [ -x "$CLIENT_DIR/mysqldump" ]; then
    MYSQLDUMP="$CLIENT_DIR/mysqldump"
else
    MYSQLDUMP=$(which mysqldump)
fi

if [ -x "$SCRIPTS_DIR/my_print_defaults" ]; then
    MY_PRINT_DEFAULTS="$SCRIPTS_DIR/my_print_defaults"
elif [ -x "$EXTRA_DIR/my_print_defaults" ]; then
    MY_PRINT_DEFAULTS="$EXTRA_DIR/my_print_defaults"
else
    MY_PRINT_DEFAULTS=$(which my_print_defaults)
fi

readonly WSREP_SST_OPT_CONF="$WSREP_SST_OPT_DEFAULT $WSREP_SST_OPT_EXTRA_DEFAULT"
MY_PRINT_DEFAULTS="$MY_PRINT_DEFAULTS $WSREP_SST_OPT_CONF"
wsrep_auth_not_set()
{
    [ -z "$WSREP_SST_OPT_AUTH" -o "$WSREP_SST_OPT_AUTH" = "(null)" ]
}

# For Bug:1200727
if $MY_PRINT_DEFAULTS sst | grep -q "wsrep_sst_auth"
then
    if wsrep_auth_not_set
    then
        WSREP_SST_OPT_AUTH=$($MY_PRINT_DEFAULTS sst | grep -- "--wsrep_sst_auth" | cut -d= -f2)
    fi
fi
readonly WSREP_SST_OPT_AUTH

# Splitting AUTH into potential user:password pair
if ! wsrep_auth_not_set
then
    readonly AUTH_VEC=(${WSREP_SST_OPT_AUTH//:/ })
    WSREP_SST_OPT_USER="${AUTH_VEC[0]:-}"
    WSREP_SST_OPT_PSWD="${AUTH_VEC[1]:-}"
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

wsrep_log_info()
{
    wsrep_log "[INFO] $*"
}

wsrep_cleanup_progress_file()
{
    [ -n "$SST_PROGRESS_FILE" ] && rm -f "$SST_PROGRESS_FILE" 2>/dev/null
}

wsrep_check_program()
{
    local prog=$1

    if ! which $prog >/dev/null
    then
        echo "'$prog' not found in PATH"
        return 2 # no such file or directory
    fi
}

wsrep_check_programs()
{
    local ret=0

    while [ $# -gt 0 ]
    do
        wsrep_check_program $1 || ret=$?
        shift
    done

    return $ret
}
