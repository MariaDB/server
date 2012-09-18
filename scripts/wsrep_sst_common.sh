# Copyright (C) 2010 Codership Oy
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

while [ $# -gt 0 ]; do
case "$1" in
    '--address')
        readonly WSREP_SST_OPT_ADDR="$2"
        shift
        ;;
    '--auth')
        readonly WSREP_SST_OPT_AUTH="$2"
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
        readonly WSREP_SST_OPT_CONF="$2"
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
        readonly WSREP_SST_OPT_PSWD="$2"
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
        readonly WSREP_SST_OPT_USER="$2"
        shift
        ;;
    '--gtid')
        readonly WSREP_SST_OPT_GTID="$2"
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

wsrep_log()
{
    # echo everything to stderr so that it gets into common error log
    # deliberately made to look different from the rest of the log
    local readonly tst="$(date +%Y%m%d\ %H:%M:%S.%N | cut -b -21)"
    echo "WSREP_SST: $* ($tst)" >>/dev/stderr
}

wsrep_log_error()
{
    wsrep_log "[ERROR] $*"
}

wsrep_log_info()
{
    wsrep_log "[INFO] $*"
}

