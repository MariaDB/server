# Copyright (C) 2017-2022 MariaDB
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
# MA  02110-1335  USA.

# This is a common command line parser to be sourced by other SST scripts

set -ue

# Setting the path for some utilities on CentOS
export PATH="$PATH:/usr/sbin:/usr/bin:/sbin:/bin"

trim_string()
{
    if [ -n "$BASH_VERSION" ]; then
        local pattern="[![:space:]${2:-}]"
        local x="${1#*$pattern}"
        local z=${#1}
        x=${#x}
        if [ $x -ne $z ]; then
            local y="${1%$pattern*}"
            y=${#y}
            x=$(( z-x-1 ))
            y=$(( y-x+1 ))
            printf '%s' "${1:$x:$y}"
        else
            printf ''
        fi
    else
        local pattern="[[:space:]${2:-}]"
        echo "$1" | sed -E "s/^$pattern+|$pattern+\$//g"
    fi
}

trim_dir()
{
    local t=$(trim_string "$1")
    if [ "$t" != '/' ]; then
        if [ "${t%/}" != "$t" ]; then
            t=$(trim_string "${t%/}")
        fi
    else
        t='.'
    fi
    if [ -n "$BASH_VERSION" ]; then
        printf '%s' "$t"
    else
        echo "$t"
    fi
}

to_minuses()
{
    local x="$1"
    local t="${1#*_}"
    local r=""
    while [ "$t" != "$x" ]; do
       r="$r${x%%_*}-"
       x="$t"
       t="${t#*_}"
    done
    if [ -n "$BASH_VERSION" ]; then
        printf '%s' "$r$x"
    else
        echo "$r$x"
    fi
}

WSREP_SST_OPT_BYPASS=0
WSREP_SST_OPT_BINLOG=""
WSREP_SST_OPT_BINLOG_INDEX=""
WSREP_SST_OPT_LOG_BASENAME=""
WSREP_SST_OPT_DATA=""
WSREP_SST_OPT_AUTH="${WSREP_SST_OPT_AUTH:-}"
WSREP_SST_OPT_USER="${WSREP_SST_OPT_USER:-}"
WSREP_SST_OPT_PSWD="${WSREP_SST_OPT_PSWD:-}"
WSREP_SST_OPT_REMOTE_AUTH="${WSREP_SST_OPT_REMOTE_AUTH:-}"
WSREP_SST_OPT_DEFAULT=""
WSREP_SST_OPT_DEFAULTS=""
WSREP_SST_OPT_EXTRA_DEFAULT=""
WSREP_SST_OPT_EXTRA_DEFAULTS=""
WSREP_SST_OPT_SUFFIX_DEFAULT=""
WSREP_SST_OPT_SUFFIX_VALUE=""
WSREP_SST_OPT_MYSQLD=""
WSREP_SST_OPT_PORT=""
WSREP_SST_OPT_ADDR=""
WSREP_SST_OPT_ADDR_PORT=""
WSREP_SST_OPT_HOST=""
WSREP_SST_OPT_HOST_UNESCAPED=""
WSREP_SST_OPT_HOST_ESCAPED=""
INNODB_DATA_HOME_DIR=$(trim_dir "${INNODB_DATA_HOME_DIR:-}")
INNODB_LOG_GROUP_HOME=$(trim_dir "${INNODB_LOG_GROUP_HOME:-}")
INNODB_UNDO_DIR=$(trim_dir "${INNODB_UNDO_DIR:-}")
INNODB_FORCE_RECOVERY=""
INNOEXTRA=""

while [ $# -gt 0 ]; do
case "$1" in
    '--address')
        WSREP_SST_OPT_ADDR="$2"
        #
        # Break address string into host:port/path parts
        #
        case "$WSREP_SST_OPT_ADDR" in
        \[*)
            # IPv6
            # Remove the starting and ending square brackets, if present:
            addr_no_bracket="${WSREP_SST_OPT_ADDR#\[}"
            # Some utilities and subsequent code require an address
            # without square brackets:
            readonly WSREP_SST_OPT_HOST_UNESCAPED="${addr_no_bracket%%\]*}"
            # Square brackets are needed in most cases:
            readonly WSREP_SST_OPT_HOST="[${WSREP_SST_OPT_HOST_UNESCAPED}]"
            readonly WSREP_SST_OPT_HOST_ESCAPED="\\[${WSREP_SST_OPT_HOST_UNESCAPED}\\]"
            # Mark this address as IPv6:
            readonly WSREP_SST_OPT_HOST_IPv6=1
            ;;
        *)
            readonly WSREP_SST_OPT_HOST="${WSREP_SST_OPT_ADDR%%[:/]*}"
            readonly WSREP_SST_OPT_HOST_UNESCAPED="$WSREP_SST_OPT_HOST"
            readonly WSREP_SST_OPT_HOST_ESCAPED="$WSREP_SST_OPT_HOST"
            readonly WSREP_SST_OPT_HOST_IPv6=0
            ;;
        esac
        # Let's remove the leading part that contains the host address:
        remain="${WSREP_SST_OPT_ADDR#$WSREP_SST_OPT_HOST_ESCAPED}"
        # Let's remove the ":" character that separates the port number
        # from the hostname:
        remain="${remain#:}"
        # Extract the port number from the address - all characters
        # up to "/" (if present):
        WSREP_SST_OPT_ADDR_PORT="${remain%%/*}"
        # If the "/" character is present, then the path is not empty:
        if [ "${remain#*/}" != "$remain" ]; then
            # This operation removes everything up to the "/" character,
            # effectively removing the port number from the string:
            readonly WSREP_SST_OPT_PATH="${remain#*/}"
        else
            readonly WSREP_SST_OPT_PATH=""
        fi
        # The rest of the string is the same as the path (for now):
        remain="$WSREP_SST_OPT_PATH"
        # If there is one more "/" in the string, then everything before
        # it will be the module name, otherwise the module name is empty:
        if [ "${remain%%/*}" != "$remain" ]; then
            # This operation removes the tail after the very first
            # occurrence of the "/" character (inclusively):
            readonly WSREP_SST_OPT_MODULE="${remain%%/*}"
        else
            readonly WSREP_SST_OPT_MODULE=""
        fi
        # Remove the module name part from the string, which ends with "/":
        remain="${WSREP_SST_OPT_PATH#*/}"
        # If the rest of the string does not match the original, then there
        # was something else besides the module name:
        if [ "$remain" != "$WSREP_SST_OPT_PATH" ]; then
            # Extract the part that matches the LSN by removing all
            # characters starting from the very first "/":
            readonly WSREP_SST_OPT_LSN="${remain%%/*}"
            # Exctract everything after the first occurrence of
            # the "/" character in the string:
            remain="${remain#*/}"
            # If the remainder does not match the original string,
            # then there is something else (the version number in
            # our case):
            if [ "$remain" != "$WSREP_SST_OPT_LSN" ]; then
                # Let's extract the version number by removing the tail
                # after the very first occurence of the "/" character
                # (inclusively):
                readonly WSREP_SST_OPT_SST_VER="${remain%%/*}"
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
        # Let's remove the trailing slash:
        readonly WSREP_SST_OPT_DATA=$(trim_dir "$2")
        shift
        ;;
    '--innodb-data-home-dir')
        # Let's remove the trailing slash:
        readonly INNODB_DATA_HOME_DIR=$(trim_dir "$2")
        shift
        ;;
    '--innodb-log-group-home-dir')
        # Let's remove the trailing slash:
        readonly INNODB_LOG_GROUP_HOME=$(trim_dir "$2")
        shift
        ;;
    '--innodb-undo-directory')
        # Let's remove the trailing slash:
        readonly INNODB_UNDO_DIR=$(trim_dir "$2")
        shift
        ;;
    '--defaults-file')
        readonly WSREP_SST_OPT_DEFAULT="$1=$2"
        readonly WSREP_SST_OPT_DEFAULTS="$1='$2'"
        shift
        ;;
    '--defaults-extra-file')
        readonly WSREP_SST_OPT_EXTRA_DEFAULT="$1=$2"
        readonly WSREP_SST_OPT_EXTRA_DEFAULTS="$1='$2'"
        shift
        ;;
    '--defaults-group-suffix')
        readonly WSREP_SST_OPT_SUFFIX_DEFAULT="$1=$2"
        readonly WSREP_SST_OPT_SUFFIX_VALUE="$2"
        shift
        ;;
    '--host')
        case "$2" in
        \[*)
            # IPv6
            # Remove the starting and ending square brackets, if present:
            addr_no_bracket="${2#\[}"
            # Some utilities and subsequent code require an address
            # without square brackets:
            readonly WSREP_SST_OPT_HOST_UNESCAPED="${addr_no_bracket%%\]*}"
            # Square brackets are needed in most cases:
            readonly WSREP_SST_OPT_HOST="[${WSREP_SST_OPT_HOST_UNESCAPED}]"
            readonly WSREP_SST_OPT_HOST_ESCAPED="\\[${WSREP_SST_OPT_HOST_UNESCAPED}\\]"
            # Mark this address as IPv6:
            readonly WSREP_SST_OPT_HOST_IPv6=1
            ;;
        *)
            readonly WSREP_SST_OPT_HOST="$2"
            readonly WSREP_SST_OPT_HOST_UNESCAPED="$2"
            readonly WSREP_SST_OPT_HOST_ESCAPED="$2"
            readonly WSREP_SST_OPT_HOST_IPv6=0
            ;;
        esac
        WSREP_SST_OPT_ADDR="$WSREP_SST_OPT_HOST"
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
    '--binlog'|'--log-bin')
        readonly WSREP_SST_OPT_BINLOG="$2"
        shift
        ;;
    '--binlog-index'|'--log-bin-index')
        WSREP_SST_OPT_BINLOG_INDEX="$2"
        shift
        ;;
    '--log-basename')
        readonly WSREP_SST_OPT_LOG_BASENAME="$2"
        shift
        ;;
    '--gtid-domain-id')
        readonly WSREP_SST_OPT_GTID_DOMAIN_ID="$2"
        shift
        ;;
    '--mysqld-args')
        original_cmd=""
        shift
        cmd_tail=0
        while [ $# -gt 0 ]; do
           lname="${1#--}"
           # "--" is interpreted as the end of the list of options:
           if [ -z "$lname" ]; then
               shift
               if [ $# -gt 0 ]; then
                   # copy "--" to the output string:
                   original_cmd="$original_cmd --"
                   # All other arguments must be copied unchanged:
                   while [ $# -gt 0 ]; do
                       original_cmd="$original_cmd '$1'"
                       shift
                   done
               fi
               break
           fi
           # Make sure the argument does not start with "--", otherwise it
           # is a long option, which is processed after this "if":
           if [ "$lname" = "$1" ]; then
               # Check if the argument is the short option or the short
               # options list, starting with "-":
               options="${1#-}"
               if [ "$options" != "$1" -a -n "$options" ]; then
                   slist=""
                   while [ -n "$options" ]; do
                       # Let's separate the first character as the current
                       # option name:
                       if [ -n "$BASH_VERSION" ]; then
                           option="${options:0:1}"
                       else
                           # If it's not bash, then we need to use slow
                           # external utilities:
                           option=$(echo "$options" | cut -c1-1)
                       fi
                       # And the subsequent characters consider option value:
                       value=""
                       if [ ${#options} -gt 0 ]; then
                           value="${options#?}"
                       fi
                       # Check for options without argument:
                       if [ "$option" != '?' -a \
                            "$option" != 'a' -a \
                            "$option" != 's' -a \
                            "$option" != 'v' ]
                       then
                           # If the option value is absent, then check
                           # the following argument:
                           if [ -z "$value" -a $# -gt 1 ]; then
                               # if the next argument does not start with
                               # the "-" character, then next argument is
                               # the current option value:
                               if [ "${2#-}" = "$2" ]; then
                                   shift
                                   value="$1"
                               elif [ "$2" = '--' ]; then
                                   shift
                                   if [ $# -gt 1 ]; then
                                       cmd_tail=1
                                       shift
                                       value="$1"
                                   fi
                               fi
                           fi
                           if [ "$option" = 'h' ]; then
                               if [ -z "$WSREP_SST_OPT_DATA" ]; then
                                   MYSQLD_OPT_DATADIR=$(trim_dir "$value")
                               fi
                           elif [ "$option" != 'u' -a \
                                  "$option" != 'P' ]
                           then
                               if [ $cmd_tail -ne 0 ]; then
                                   option="$option --"
                               fi
                               if [ -z "$value" ]; then
                                   slist="$slist$option"
                               elif [ -z "$slist" ]; then
                                   slist="$option '$value'"
                               else
                                   slist="$slist -$option '$value'"
                               fi
                               break
                           fi
                           if [ $cmd_tail -ne 0 ]; then
                               if [ -n "$slist" ]; then
                                   slist="$slist --"
                               else
                                   slist='-'
                               fi
                           fi
                           break
                       else
                           slist="$slist$option"
                       fi
                       options="$value"
                   done
                   if [ -n "$slist" ]; then
                       original_cmd="$original_cmd -$slist"
                   fi
               elif [ -z "$options" ]; then
                   # We found an minus sign without any characters after it:
                   original_cmd="$original_cmd -"
               else
                   # We found a value that does not start with a minus -
                   # it is a positional argument or the value of previous
                   # option. Copy it to output string (as is):
                   original_cmd="$original_cmd '$1'"
               fi
               shift
               if [ $cmd_tail -ne 0 ]; then
                   # All other arguments must be copied unchanged:
                   while [ $# -gt 0 ]; do
                       original_cmd="$original_cmd '$1'"
                       shift
                   done
                   break
               fi
               continue
           fi
           # Now we are sure that we are working with an option
           # that has a "long" name, so remove all characters after
           # the first equal sign:
           option="${1%%=*}"
           # If the option name contains underscores, then replace
           # them to minuses:
           if [ "${option#*_}" != "$option" ]; then
               option=$(to_minuses "$option")
           fi
           # The "--loose-" prefix should not affect the recognition
           # of the option name:
           if [ "${option#--loose-}" != "$option" ]; then
               option="--${option#--loose-}"
           fi
           # Some options just need to be removed from the list:
           if [ "$option" != '--defaults-file' -a \
                "$option" != '--defaults-extra-file' -a \
                "$option" != '--defaults-group-suffix' -a \
                "$option" != '--user' -a \
                "$option" != '--port' -a \
                "$option" != '--socket' ]; then
               value="${1#*=}"
               if [ "$value" = "$1" ]; then
                   value=""
               fi
               # Let's fill in the variables containing important paths
               # that might not have been passed through explicit parameters
               # (+ removing the trailing slash in these paths). Many of these
               # options are processed internally within scripts or passed
               # explicitly to other programs, so we need to remove them
               # from mysqld's argument list:
               skip_mysqld_arg=0
               case "$option" in
                   '--innodb-data-home-dir')
                       if [ -z "$INNODB_DATA_HOME_DIR" ]; then
                           MYSQLD_OPT_INNODB_DATA_HOME_DIR=$(trim_dir "$value")
                       fi
                       skip_mysqld_arg=1
                       ;;
                   '--innodb-log-group-home-dir')
                       if [ -z "$INNODB_LOG_GROUP_HOME" ]; then
                           MYSQLD_OPT_INNODB_LOG_GROUP_HOME=$(trim_dir "$value")
                       fi
                       skip_mysqld_arg=1
                       ;;
                   '--innodb-undo-directory')
                       if [ -z "$INNODB_UNDO_DIR" ]; then
                           MYSQLD_OPT_INNODB_UNDO_DIR=$(trim_dir "$value")
                       fi
                       skip_mysqld_arg=1
                       ;;
                   '--innodb-force-recovery')
                       if [ -n "$value" -a "$value" != "0" ]; then
                           INNODB_FORCE_RECOVERY="$value"
                       fi
                       skip_mysqld_arg=1
                       ;;
                   '--log-bin')
                       if [ -z "$WSREP_SST_OPT_BINLOG" ]; then
                           MYSQLD_OPT_LOG_BIN="$value"
                       fi
                       skip_mysqld_arg=1
                       ;;
                   '--log-bin-index')
                       if [ -z "$WSREP_SST_OPT_BINLOG_INDEX" ]; then
                           MYSQLD_OPT_LOG_BIN_INDEX="$value"
                       fi
                       skip_mysqld_arg=1
                       ;;
                   '--log-basename')
                       if [ -z "$WSREP_SST_OPT_LOG_BASENAME" ]; then
                           MYSQLD_OPT_LOG_BASENAME="$value"
                       fi
                       skip_mysqld_arg=1
                       ;;
                   '--datadir')
                       if [ -z "$WSREP_SST_OPT_DATA" ]; then
                           MYSQLD_OPT_DATADIR=$(trim_dir "$value")
                       fi
                       skip_mysqld_arg=1
                       ;;
               esac
               if [ $skip_mysqld_arg -eq 0 ]; then
                   original_cmd="$original_cmd '$1'"
               fi
           fi
           shift
        done
        WSREP_SST_OPT_MYSQLD="${original_cmd# *}"
        break
        ;;
    *) # Must be command usage
        # exit 1
        ;;
esac
shift
done
readonly WSREP_SST_OPT_BYPASS

# The same argument can be present on the command line several
# times, in this case we must take its last value:
if [ -n "${MYSQLD_OPT_INNODB_DATA_HOME_DIR:-}" -a \
     -z "$INNODB_DATA_HOME_DIR" ]; then
    readonly INNODB_DATA_HOME_DIR="$MYSQLD_OPT_INNODB_DATA_HOME_DIR"
fi
if [ -n "${MYSQLD_OPT_INNODB_LOG_GROUP_HOME:-}" -a \
     -z "$INNODB_LOG_GROUP_HOME" ]; then
    readonly INNODB_LOG_GROUP_HOME="$MYSQLD_OPT_INNODB_LOG_GROUP_HOME"
fi
if [ -n "${MYSQLD_OPT_INNODB_UNDO_DIR:-}" -a \
     -z "$INNODB_UNDO_DIR" ]; then
    readonly INNODB_UNDO_DIR="$MYSQLD_OPT_INNODB_UNDO_DIR"
fi
if [ -n "${MYSQLD_OPT_LOG_BIN:-}" -a \
     -z "$WSREP_SST_OPT_BINLOG" ]; then
    readonly WSREP_SST_OPT_BINLOG="$MYSQLD_OPT_LOG_BIN"
fi
if [ -n "${MYSQLD_OPT_LOG_BIN_INDEX:-}" -a \
     -z "$WSREP_SST_OPT_BINLOG_INDEX" ]; then
    WSREP_SST_OPT_BINLOG_INDEX="$MYSQLD_OPT_LOG_BIN_INDEX"
fi
if [ -n "${MYSQLD_OPT_DATADIR:-}" -a \
     -z "$WSREP_SST_OPT_DATA" ]; then
    readonly WSREP_SST_OPT_DATA="$MYSQLD_OPT_DATADIR"
fi
if [ -n "${MYSQLD_OPT_LOG_BASENAME:-}" -a \
     -z "$WSREP_SST_OPT_LOG_BASENAME" ]; then
    readonly WSREP_SST_OPT_LOG_BASENAME="$MYSQLD_OPT_LOG_BASENAME"
fi

# If the --log-bin option is present without a value, then
# set WSREP_SST_OPT_BINLOG value using other arguments:
if [ -z "$WSREP_SST_OPT_BINLOG" -a -n "${MYSQLD_OPT_LOG_BIN+x}" ]; then
    if [ -n "$WSREP_SST_OPT_LOG_BASENAME" ]; then
        # If the WSREP_SST_OPT_BINLOG variable is not set, but
        # --log-basename is present among the arguments to mysqld,
        # then set WSREP_SST_OPT_BINLOG equal to the base name
        # with the "-bin" suffix:
        readonly WSREP_SST_OPT_BINLOG="$WSREP_SST_OPT_LOG_BASENAME-bin"
    else
        # Take the default name:
        readonly WSREP_SST_OPT_BINLOG='mysql-bin'
    fi
fi

# Reconstructing the command line arguments that control the innodb
# and binlog options:
if [ -n "$WSREP_SST_OPT_LOG_BASENAME" ]; then
    if [ -n "$WSREP_SST_OPT_MYSQLD" ]; then
        WSREP_SST_OPT_MYSQLD="--log-basename='$WSREP_SST_OPT_LOG_BASENAME' $WSREP_SST_OPT_MYSQLD"
    else
        WSREP_SST_OPT_MYSQLD="--log-basename='$WSREP_SST_OPT_LOG_BASENAME'"
    fi
fi
if [ -n "$INNODB_DATA_HOME_DIR" ]; then
    INNOEXTRA="$INNOEXTRA --innodb-data-home-dir='$INNODB_DATA_HOME_DIR'"
fi
if [ -n "$INNODB_LOG_GROUP_HOME" ]; then
    INNOEXTRA="$INNOEXTRA --innodb-log-group-home-dir='$INNODB_LOG_GROUP_HOME'"
fi
if [ -n "$INNODB_UNDO_DIR" ]; then
    INNOEXTRA="$INNOEXTRA --innodb-undo-directory='$INNODB_UNDO_DIR'"
fi
if [ -n "$WSREP_SST_OPT_BINLOG" ]; then
    INNOEXTRA="$INNOEXTRA --log-bin='$WSREP_SST_OPT_BINLOG'"
    if [ -n "$WSREP_SST_OPT_BINLOG_INDEX" ]; then
        if [ -n "$WSREP_SST_OPT_MYSQLD" ]; then
            WSREP_SST_OPT_MYSQLD="--log-bin-index='$WSREP_SST_OPT_BINLOG_INDEX' $WSREP_SST_OPT_MYSQLD"
        else
            WSREP_SST_OPT_MYSQLD="--log-bin-index='$WSREP_SST_OPT_BINLOG_INDEX'"
        fi
    fi
fi

readonly INNODB_FORCE_RECOVERY
readonly WSREP_SST_OPT_MYSQLD

get_binlog()
{
    # if no command line argument and WSREP_SST_OPT_BINLOG is not set,
    # try to get it from my.cnf:
    if [ -z "$WSREP_SST_OPT_BINLOG" ]; then
        WSREP_SST_OPT_BINLOG=$(parse_cnf '--mysqld' 'log-bin')
    fi
    # if no command line argument and WSREP_SST_OPT_BINLOG_INDEX is not set,
    # try to get it from my.cnf:
    if [ -z "$WSREP_SST_OPT_BINLOG_INDEX" ]; then
        WSREP_SST_OPT_BINLOG_INDEX=$(parse_cnf '--mysqld' 'log-bin-index')
    fi
    # if no command line argument and WSREP_SST_OPT_LOG_BASENAME is not set,
    # then try to get it from my.cnf:
    if [ -z "$WSREP_SST_OPT_LOG_BASENAME" ]; then
        WSREP_SST_OPT_LOG_BASENAME=$(parse_cnf '--mysqld' 'log-basename')
    fi
    if [ -z "$WSREP_SST_OPT_BINLOG" ]; then
        # If the log-bin option is specified without a parameter,
        # then we need to build the name of the index file according
        # to the rules described in the server documentation:
        if [ $(in_config '--mysqld' 'log-bin') -ne 0 ]; then
            if [ -n "$WSREP_SST_OPT_LOG_BASENAME" ]; then
                # If the WSREP_SST_OPT_BINLOG variable is not set, but
                # --log-basename is present among the arguments of mysqld,
                # then set WSREP_SST_OPT_BINLOG equal to the base name
                # with the "-bin" suffix:
                readonly WSREP_SST_OPT_BINLOG="$WSREP_SST_OPT_LOG_BASENAME-bin"
            else
                # Take the default name:
                readonly WSREP_SST_OPT_BINLOG='mysql-bin'
            fi
        fi
    fi
    if [ -n "$WSREP_SST_OPT_BINLOG" ]; then
        # If the name of the index file is not specified, then we will build
        # it according to the specifications for the server:
        if [ -z "$WSREP_SST_OPT_BINLOG_INDEX" ]; then
            if [ -n "$WSREP_SST_OPT_LOG_BASENAME" ]; then
                # If the WSREP_SST_OPT_BINLOG_INDEX variable is not set, but
                # --log-basename is present among the arguments of mysqld,
                # then set WSREP_SST_OPT_BINLOG_INDEX equal to the base name
                # with the "-bin" suffix:
                readonly WSREP_SST_OPT_BINLOG_INDEX="$WSREP_SST_OPT_LOG_BASENAME-bin.index"
            else
                # Use the default name (note that base of this name
                # is already defined above):
                readonly WSREP_SST_OPT_BINLOG_INDEX="$WSREP_SST_OPT_BINLOG.index"
            fi
        else
            # Remove all directories from the index file path:
            local filename="${WSREP_SST_OPT_BINLOG_INDEX##*/}"
            # Check if the index file name contains the extension:
            if [ "${filename%.*}" = "$filename" ]; then
                # Let's add the default extension (".index"):
                readonly WSREP_SST_OPT_BINLOG_INDEX="$WSREP_SST_OPT_BINLOG_INDEX.index"
            else
                readonly WSREP_SST_OPT_BINLOG_INDEX
            fi
        fi
    fi
}

# Check the presence of the port value and, if necessary, transfer
# the port number from the address to the WSREP_SST_OPT_PORT variable
# or vice versa, and also, if necessary, substitute the missing port
# value into the address value:
if [ -n "$WSREP_SST_OPT_ADDR_PORT" ]; then
    if [ -n "$WSREP_SST_OPT_PORT" ]; then
        if [ "$WSREP_SST_OPT_PORT" != "$WSREP_SST_OPT_ADDR_PORT" ]; then
            echo "WSREP_SST: [ERROR] port in --port=$WSREP_SST_OPT_PORT" \
                 "differs from port in --address=$WSREP_SST_OPT_ADDR" >&2
            exit 2
        fi
    else
        # If the address contains a port number, assign it to
        # the corresponding variable:
        readonly WSREP_SST_OPT_PORT="$WSREP_SST_OPT_ADDR_PORT"
    fi
elif [ -n "$WSREP_SST_OPT_ADDR" ]; then
    # If the port is missing, take the default port:
    if [ -z "$WSREP_SST_OPT_PORT" ]; then
        readonly WSREP_SST_OPT_PORT=4444
    fi
    WSREP_SST_OPT_ADDR_PORT="$WSREP_SST_OPT_PORT"
    # Let's remove the leading part that contains the host address:
    remain="${WSREP_SST_OPT_ADDR#$WSREP_SST_OPT_HOST_ESCAPED}"
    # Let's remove the ":" character that separates the port number
    # from the hostname:
    remain="${remain#:}"
    # Let's remove all characters upto first "/" character that
    # separates the hostname with port number from the path:
    remain="${remain#/}"
    # Let's construct a new value for the address with the port:
    WSREP_SST_OPT_ADDR="$WSREP_SST_OPT_HOST:$WSREP_SST_OPT_PORT"
    if [ -n "$remain" ]; then
        WSREP_SST_OPT_ADDR="$WSREP_SST_OPT_ADDR/$remain"
    fi
fi

readonly WSREP_SST_OPT_ADDR
readonly WSREP_SST_OPT_ADDR_PORT

commandex()
{
    if [ -n "$BASH_VERSION" ]; then
        command -v "$1" || :
    elif [ -x "$1" ]; then
        echo "$1"
    else
        which "$1" || :
    fi
}

# try to use my_print_defaults, mysql and mysqldump that come
# with the sources (for MTR suite):
script_binary=$(dirname "$0")
SCRIPTS_DIR=$(cd "$script_binary"; pwd)
EXTRA_DIR="$SCRIPTS_DIR/../extra"
CLIENT_DIR="$SCRIPTS_DIR/../client"

if [ -x "$CLIENT_DIR/mysql" ]; then
    MYSQL_CLIENT="$CLIENT_DIR/mysql"
else
    MYSQL_CLIENT=$(commandex 'mysql')
fi

if [ -x "$CLIENT_DIR/mysqldump" ]; then
    MYSQLDUMP="$CLIENT_DIR/mysqldump"
else
    MYSQLDUMP=$(commandex 'mysqldump')
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

if [ -x "$SCRIPTS_DIR/my_print_defaults" ]; then
    MY_PRINT_DEFAULTS="$SCRIPTS_DIR/my_print_defaults"
elif [ -x "$EXTRA_DIR/my_print_defaults" ]; then
    MY_PRINT_DEFAULTS="$EXTRA_DIR/my_print_defaults"
else
    MY_PRINT_DEFAULTS=$(commandex 'my_print_defaults')
    if [ -z "$MY_PRINT_DEFAULTS" ]; then
        wsrep_log_error "my_print_defaults not found in path"
        exit 2
    fi
fi

readonly MY_PRINT_DEFAULTS

wsrep_defaults="$WSREP_SST_OPT_DEFAULTS"
wsrep_defaults="$wsrep_defaults${WSREP_SST_OPT_EXTRA_DEFAULTS:+ }$WSREP_SST_OPT_EXTRA_DEFAULTS"
wsrep_defaults="$wsrep_defaults${WSREP_SST_OPT_SUFFIX_DEFAULT:+ }$WSREP_SST_OPT_SUFFIX_DEFAULT"

readonly WSREP_SST_OPT_CONF="${wsrep_defaults:+ }$wsrep_defaults"

wsrep_defaults="$WSREP_SST_OPT_DEFAULT"
wsrep_defaults="$wsrep_defaults${WSREP_SST_OPT_EXTRA_DEFAULT:+ }$WSREP_SST_OPT_EXTRA_DEFAULT"
wsrep_defaults="$wsrep_defaults${WSREP_SST_OPT_SUFFIX_DEFAULT:+ }$WSREP_SST_OPT_SUFFIX_DEFAULT"

readonly WSREP_SST_OPT_CONF_UNQUOTED="${wsrep_defaults:+ }$wsrep_defaults"

#
# User can specify mariabackup specific settings that will be used during sst
# process like encryption, etc. Parse such configuration option.
#
# 1st parameter: group (config file section like sst) or
#                my_print_defaults argument (like --mysqld)
# 2nd parameter: var : name of the variable in the section, e.g. server-id
# 3rd parameter: default value for the parameter
#
parse_cnf()
{
    local groups="$1"
    local var="$2"
    local reval=""

    # normalize the variable names specified in the .cnf file
    # (user can use '_' or '-', for example, log-bin or log_bin),
    # then search for the last instance of the desired variable
    # and finally get the value of that variable (if the variable
    # was specified several times - we use only its last instance):

    local pattern='BEGIN {OFS=FS="="} {sub(/^--loose/,"-",$0); gsub(/_/,"-",$1); if ($1=="--'"$var"'") lastval=substr($0,length($1)+2)} END {print lastval}'

    while [ -n "$groups" ]; do
        # Remove the largest suffix starting with the '|' character:
        local group="${groups%%\|*}"
        # Remove the remainder (the group name) from the rest
        # of the groups list (as if it were a prefix):
        groups="${groups#$group}"
        groups="${groups#\|}"
        # If the group name is the same as the "mysqld" without "--" prefix,
        # then try to use it together with the group suffix:
        if [ "$group" = 'mysqld' -a -n "$WSREP_SST_OPT_SUFFIX_VALUE" ]; then
            reval=$("$MY_PRINT_DEFAULTS" \
                    ${WSREP_SST_OPT_DEFAULT:+"$WSREP_SST_OPT_DEFAULT"} \
                    ${WSREP_SST_OPT_EXTRA_DEFAULT:+"$WSREP_SST_OPT_EXTRA_DEFAULT"} \
                    ${WSREP_SST_OPT_SUFFIX_DEFAULT:+"$WSREP_SST_OPT_SUFFIX_DEFAULT"} \
                    "mysqld$WSREP_SST_OPT_SUFFIX_VALUE" | awk "$pattern")
            if [ -n "$reval" ]; then
                break
            fi
        fi
        # Let's try to use the group name as it is:
        reval=$("$MY_PRINT_DEFAULTS" \
                ${WSREP_SST_OPT_DEFAULT:+"$WSREP_SST_OPT_DEFAULT"} \
                ${WSREP_SST_OPT_EXTRA_DEFAULT:+"$WSREP_SST_OPT_EXTRA_DEFAULT"} \
                ${WSREP_SST_OPT_SUFFIX_DEFAULT:+"$WSREP_SST_OPT_SUFFIX_DEFAULT"} \
                "$group" | awk "$pattern")
        if [ -n "$reval" ]; then
            break
        fi
    done

    # Use default if we haven't found a value:
    if [ -z "$reval" ]; then
        [ -n "${3:-}" ] && reval="$3"
    fi
    if [ -n "$BASH_VERSION" ]; then
        printf '%s' "$reval"
    else
        echo "$reval"
    fi
}

#
# This function simply checks for the presence of the parameter
# in the config file, but does not return its value. It returns "1"
# (true) even if the parameter is present in the configuration file
# without a value:
#
in_config()
{
    local groups="$1"
    local var="$2"
    local found=0

    # normalize the variable names specified in the .cnf file
    # (user can use '_' or '-', for example, log-bin or log_bin),
    # then search for the last instance(s) of the desired variable:

    local pattern='BEGIN {OFS=FS="="; found=0} {sub(/^--loose/,"-",$0); gsub(/_/,"-",$1); if ($1=="--'"$var"'") found=1} END {print found}'

    while [ -n "$groups" ]; do
        # Remove the largest suffix starting with the '|' character:
        local group="${groups%%\|*}"
        # Remove the remainder (the group name) from the rest
        # of the groups list (as if it were a prefix):
        groups="${groups#$group}"
        groups="${groups#\|}"
        # If the group name is the same as the "mysqld" without "--" prefix,
        # then try to use it together with the group suffix:
        if [ "$group" = 'mysqld' -a -n "$WSREP_SST_OPT_SUFFIX_VALUE" ]; then
            found=$("$MY_PRINT_DEFAULTS" \
                    ${WSREP_SST_OPT_DEFAULT:+"$WSREP_SST_OPT_DEFAULT"} \
                    ${WSREP_SST_OPT_EXTRA_DEFAULT:+"$WSREP_SST_OPT_EXTRA_DEFAULT"} \
                    ${WSREP_SST_OPT_SUFFIX_DEFAULT:+"$WSREP_SST_OPT_SUFFIX_DEFAULT"} \
                    "mysqld$WSREP_SST_OPT_SUFFIX_VALUE" | awk "$pattern")
            if [ $found -ne 0 ]; then
                break
            fi
        fi
        # Let's try to use the group name as it is:
        found=$($MY_PRINT_DEFAULTS \
                ${WSREP_SST_OPT_DEFAULT:+"$WSREP_SST_OPT_DEFAULT"} \
                ${WSREP_SST_OPT_EXTRA_DEFAULT:+"$WSREP_SST_OPT_EXTRA_DEFAULT"} \
                ${WSREP_SST_OPT_SUFFIX_DEFAULT:+"$WSREP_SST_OPT_SUFFIX_DEFAULT"} \
                "$group" | awk "$pattern")
        if [ $found -ne 0 ]; then
            break
        fi
    done
    if [ -n "$BASH_VERSION" ]; then
        printf '%s' $found
    else
        echo $found
    fi
}

wsrep_auth_not_set()
{
    [ -z "$WSREP_SST_OPT_AUTH" ]
}

# Get rid of incorrect values resulting from substitution
# in programs external to the script:
if [ "$WSREP_SST_OPT_USER" = '(null)' ]; then
    WSREP_SST_OPT_USER=""
fi
if [ "$WSREP_SST_OPT_PSWD" = '(null)' ]; then
    WSREP_SST_OPT_PSWD=""
fi
if [ "$WSREP_SST_OPT_AUTH" = '(null)' ]; then
    WSREP_SST_OPT_AUTH=""
fi

# Let's read the value of the authentication string from the
# configuration file so that it does not go to the command line
# and does not appear in the ps output:
if wsrep_auth_not_set; then
    WSREP_SST_OPT_AUTH=$(parse_cnf 'sst' 'wsrep-sst-auth')
fi

# Splitting WSREP_SST_OPT_AUTH as "user:password" pair:
if ! wsrep_auth_not_set; then
    # Extract username as shortest prefix up to first ':' character:
    WSREP_SST_OPT_AUTH_USER="${WSREP_SST_OPT_AUTH%%:*}"
    if [ -z "$WSREP_SST_OPT_USER" ]; then
        # if the username is not in the command line arguments,
        # set the username and password using WSREP_SST_OPT_AUTH
        # from the environment:
        WSREP_SST_OPT_USER="$WSREP_SST_OPT_AUTH_USER"
        WSREP_SST_OPT_PSWD="${WSREP_SST_OPT_AUTH#*:}"
    elif [ "$WSREP_SST_OPT_USER" = "$WSREP_SST_OPT_AUTH_USER" ]; then
        # If the username in the command line arguments and in
        # the environment variable are the same, set the password
        # if it was not specified in the command line:
        if [ -z "$WSREP_SST_OPT_PSWD" ]; then
            WSREP_SST_OPT_PSWD="${WSREP_SST_OPT_AUTH#*:}"
        fi
    else
        # The username is passed through the command line and does
        # not match the username in the environment variable - ignore
        # the environment and rebuild the authentication parameters:
        WSREP_SST_OPT_AUTH="$WSREP_SST_OPT_USER:$WSREP_SST_OPT_PSWD"
    fi
fi

readonly WSREP_SST_OPT_USER
readonly WSREP_SST_OPT_PSWD
readonly WSREP_SST_OPT_AUTH

if [ -n "$WSREP_SST_OPT_REMOTE_AUTH" ]; then
    # Split auth string at the last ':'
    readonly WSREP_SST_OPT_REMOTE_USER="${WSREP_SST_OPT_REMOTE_AUTH%%:*}"
    readonly WSREP_SST_OPT_REMOTE_PSWD="${WSREP_SST_OPT_REMOTE_AUTH#*:}"
else
    readonly WSREP_SST_OPT_REMOTE_USER=
    readonly WSREP_SST_OPT_REMOTE_PSWD=
fi

readonly WSREP_SST_OPT_REMOTE_AUTH

if [ -n "$WSREP_SST_OPT_DATA" ]; then
    SST_PROGRESS_FILE="$WSREP_SST_OPT_DATA/sst_in_progress"
else
    SST_PROGRESS_FILE=""
fi

wsrep_cleanup_progress_file()
{
    if [ -n "$SST_PROGRESS_FILE" -a -f "$SST_PROGRESS_FILE" ]; then
        rm -f "$SST_PROGRESS_FILE" 2>/dev/null || :
    fi
}

wsrep_check_program()
{
    local prog="$1"
    local cmd=$(commandex "$prog")
    if [ -z "$cmd" ]; then
        echo "'$prog' not found in PATH"
        return 2 # no such file or directory
    fi
}

wsrep_check_programs()
{
    local ret=0
    while [ $# -gt 0 ]; do
        wsrep_check_program "$1" || ret=$?
        shift
    done
    return $ret
}

wsrep_check_datadir()
{
    if [ -z "$WSREP_SST_OPT_DATA" ]; then
        wsrep_log_error \
            "The '--datadir' parameter must be passed to the SST script"
        exit 2
    fi
}

get_openssl()
{
    # If the OPENSSL_BINARY variable is already defined, just return:
    if [ -n "${OPENSSL_BINARY+x}" ]; then
        return
    fi
    # Let's look for openssl:
    OPENSSL_BINARY=$(commandex 'openssl')
    if [ -z "$OPENSSL_BINARY" ]; then
        OPENSSL_BINARY='/usr/bin/openssl'
        if [ ! -x "$OPENSSL_BINARY" ]; then
            OPENSSL_BINARY=""
        fi
    fi
    readonly OPENSSL_BINARY
}

#
# Generate a string equivalent to 16 random bytes
#
wsrep_gen_secret()
{
    get_openssl
    if [ -n "$OPENSSL_BINARY" ]; then
        "$OPENSSL_BINARY" rand -hex 16
    elif [ -n "$BASH_VERSION" ]; then
        printf '%04x%04x%04x%04x%04x%04x%04x%04x' \
               $RANDOM $RANDOM $RANDOM $RANDOM \
               $RANDOM $RANDOM $RANDOM $RANDOM
    elif [ -n "$(commandex cksum)" -a \
           -n "$(commandex printf)" ]
    then
        printf '%08x%08x%08x%08x' \
            $(head -8 /dev/urandom | cksum | cut -d ' ' -f1) \
            $(head -8 /dev/urandom | cksum | cut -d ' ' -f1) \
            $(head -8 /dev/urandom | cksum | cut -d ' ' -f1) \
            $(head -8 /dev/urandom | cksum | cut -d ' ' -f1)
    else
        wsrep_log_error "Unable to generate 16-byte secret"
        exit 22
    fi
}

#
# Checking if the address passed to us is local.
# If the second parameter is nonzero, then this function
# does not check for matches with local domain names:
#
is_local_ip()
{
    # Rapid recognition of the most common cases:
    [ "$1" = '127.0.0.1' -o \
      "$1" = '127.0.0.2' -o \
      "$1" = 'localhost' -o \
      "$1" = '[::1]' ] && return 0
    # If the address starts with "127." this is probably a local
    # address, but we need to clarify what follows this prefix:
    if [ "${1#127.}" != "$1" ]; then
        # All 127.0.0.0/8 addresses are local:
        if echo "$1" | grep -q -E '^127\.[0-9]+\.[0-9]+\.[0-9]+$'; then
            return 0
        fi
    fi
    # If the second parameter is nonzero, then we will skip
    # the domain name check:
    if [ "${2:-0}" -eq 0 ]; then
       # We consider all the names of a given host to be local addresses:
       [ "$1" = "$(hostname -s)" -o \
         "$1" = "$(hostname -f)" -o \
         "$1" = "$(hostname -d)" ] && return 0
    fi
    # Now let's check if the given address is assigned to
    # one of the network cards:
    local ip_util=$(commandex 'ip')
    if [ -n "$ip_util" ]; then
        # ip address show ouput format is " inet[6] <address>/<mask>":
        "$ip_util" address show \
             | grep -E '^[[:space:]]*inet.? [^[:space:]]+/' -o \
             | grep -F " $1/" >/dev/null && return 0
    else
        local ifconfig_util=$(commandex 'ifconfig')
        if [ -n "$ifconfig_util" ]; then
            # ifconfig output format is " inet[6] <address> ...":
            "$ifconfig_util" \
                 | grep -E '^[[:space:]]*inet.? [^[:space:]]+ ' -o \
                 | grep -F " $1 " >/dev/null && return 0
        fi
    fi
    return 1
}

check_sockets_utils()
{
    lsof_available=0
    sockstat_available=0
    ss_available=0

    [ -n "$(commandex lsof)" ] && lsof_available=1
    [ -n "$(commandex sockstat)" ] && sockstat_available=1
    [ -n "$(commandex ss)" ] && ss_available=1

    if [ $lsof_available -eq 0 -a \
         $sockstat_available -eq 0 -a \
         $ss_available -eq 0 ]
    then
        wsrep_log_error "Neither lsof, nor sockstat or ss tool was found in" \
                        "the PATH. Make sure you have it installed."
        exit 2 # ENOENT
    fi
}

#
# Check if the port is in the "listen" state.
# The first parameter is the PID of the process that should
# listen on the port - if it is not known, you can specify
# an empty string or zero.
# The second parameter is the port number.
# The third parameter is a list of the names of utilities
# (via "|") that can listen on this port during the state
# transfer.
#
check_port()
{
    local pid="${1:-0}"
    local port="$2"
    local utils="$3"

    [ $pid -le 0 ] && pid='[0-9]+'

    local rc=1

    if [ $lsof_available -ne 0 ]; then
        lsof -Pnl -i ":$port" 2>/dev/null | \
        grep -q -E "^($utils)[^[:space:]]*[[:space:]]+$pid[[:space:]].*\\(LISTEN\\)" && rc=0
    elif [ $sockstat_available -ne 0 ]; then
        local opts='-p'
        if [ "$OS" = 'FreeBSD' ]; then
            # sockstat on FreeBSD requires the "-s" option
            # to display the connection state:
            opts='-sp'
        fi
        sockstat "$opts" "$port" 2>/dev/null | \
        grep -q -E "[[:space:]]+($utils)[^[:space:]]*[[:space:]]+$pid[[:space:]].*[[:space:]]LISTEN" && rc=0
    elif [ $ss_available -ne 0 ]; then
        ss -nlpH "( sport = :$port )" 2>/dev/null | \
        grep -q -E "users:\\(.*\\(\"($utils)[^[:space:]]*\"[^)]*,pid=$pid(,[^)]*)?\\)" && rc=0
    else
        wsrep_log_error "Unknown sockets utility"
        exit 2 # ENOENT
    fi

    return $rc
}

#
# If the ssl_dhparams variable is already set, uses that as a source
# of dh parameters for OpenSSL. Otherwise, looks for dhparams.pem in
# the datadir, and creates it there if it can't find the file.
#
check_for_dhparams()
{
    ssl_dhparams="$DATA/dhparams.pem"
    if [ ! -r "$ssl_dhparams" ]; then
        get_openssl
        if [ -n "$OPENSSL_BINARY" ]; then
            wsrep_log_info \
                "Could not find dhparams file, creating $ssl_dhparams"
            local bug=0
            local errmsg
            errmsg=$("$OPENSSL_BINARY" \
                         dhparam -out "$ssl_dhparams" 2048 2>&1) || bug=1
            if [ $bug -ne 0 ]; then
                wsrep_log_info "run: \"$OPENSSL_BINARY\" dhparam -out \"$ssl_dhparams\" 2048"
                wsrep_log_info "output: $errmsg"
                wsrep_log_error "******** ERROR *****************************************"
                wsrep_log_error "* Could not create the dhparams.pem file with OpenSSL. *"
                wsrep_log_error "********************************************************"
                ssl_dhparams=""
            fi
        else
            # Rollback: if openssl is not installed, then use
            # the default parameters:
            ssl_dhparams=""
        fi
    fi
}

#
# Verifies that the CA file verifies the certificate.
# Doing this here lets us generate better error messages.
#
# 1st param: path to the CA file.
# 2nd param: path to the certificate.
#
verify_ca_matches_cert()
{
    local cert="$1"
    local ca="$2"
    local cap="$3"

    local readable=1; [ ! -r "$cert" ] && readable=0
         [ -n "$ca"  -a ! -r "$ca"   ] && readable=0
         [ -n "$cap" -a ! -r "$cap"  ] && readable=0

    if [ $readable -eq 0 ]; then
        wsrep_log_error \
            "Both PEM file and CA file (or path) must be readable"
        exit 22
    fi

    # If the openssl utility is not installed, then
    # we will not do this certificate check:
    get_openssl
    if [ -z "$OPENSSL_BINARY" ]; then
        wsrep_log_info "openssl utility not found"
        return
    fi

    local not_match=0
    local errmsg
    errmsg=$("$OPENSSL_BINARY" verify -verbose \
                 ${ca:+ -CAfile} ${ca:+ "$ca"} \
                 ${cap:+ -CApath} ${cap:+ "$cap"} \
                 "$cert" 2>&1) || not_match=1

    if [ $not_match -eq 1 ]; then
        wsrep_log_info "run: \"$OPENSSL_BINARY\" verify -verbose${ca:+ -CAfile \"$ca\"}${cap:+ -CApath \"$cap\"} \"$cert\""
        wsrep_log_info "output: $errmsg"
        wsrep_log_error "******** FATAL ERROR ********************************************"
        wsrep_log_error "* The certifcate and CA (certificate authority) do not match.   *"
        wsrep_log_error "* It does not appear that the certificate was issued by the CA. *"
        wsrep_log_error "* Please check your certificate and CA files.                   *"
        wsrep_log_error "*****************************************************************"
        exit 22
    fi
}

#
# Verifies that the certificate matches the private key.
# Doing this will save us having to wait for a timeout that would
# otherwise occur.
#
# 1st param: path to the certificate.
# 2nd param: path to the private key.
#
verify_cert_matches_key()
{
    local cert="$1"
    local key="$2"

    if [ ! -r "$key" -o ! -r "$cert" ]; then
        wsrep_log_error "Both the certificate file and the key file" \
                        "must be readable"
        exit 22
    fi

    # If the openssl utility is not installed, then
    # we will not do this certificate check:
    get_openssl
    if [ -z "$OPENSSL_BINARY" ]; then
        wsrep_log_info "openssl utility not found"
        return
    fi

    # Generate the public key from the cert and the key.
    # They should match (otherwise we can't create an SSL connection).
    local pk1=$("$OPENSSL_BINARY" x509 -in "$cert" -pubkey -noout 2>/dev/null || :)
    local pk2=$("$OPENSSL_BINARY" pkey -in "$key" -pubout 2>/dev/null || :)
    if [ "$pk1" != "$pk2" ]; then
        wsrep_log_error "******************* FATAL ERROR *****************"
        wsrep_log_error "* The certificate and private key do not match. *"
        wsrep_log_error "* Please check your certificate and key files.  *"
        wsrep_log_error "*************************************************"
        exit 22
    fi
}

#
# Compares two version strings.
# The first parameter is the version to be checked;
# The second parameter is the minimum version required;
# Returns 1 (failure) if $1 >= $2, 0 (success) otherwise.
#
check_for_version()
{
    y1="${1#*.}"
    [ "$y1" = "$1" ] && y1=""
    z1=${y1#*.}
    [ "$z1" = "$y1" ] && z1=""
    x1="${1%%.*}"
    y1="${y1%%.*}"
    z1="${z1%%.*}"
    [ -z "$y1" ] && y1=0
    [ -z "$z1" ] && z1=0
    y2="${2#*.}"
    [ "$y2" = "$2" ] && y2=""
    z2="${y2#*.}"
    [ "$z2" = "$y2" ] && z2=""
    x2="${2%%.*}"
    y2="${y2%%.*}"
    z2="${z2%%.*}"
    [ -z "$y2" ] && y2=0
    [ -z "$z2" ] && z2=0
    [ $x1 -lt $x2 ] && return 1
    [ $x1 -gt $x2 ] && return 0
    [ $y1 -lt $y2 ] && return 1
    [ $y1 -gt $y2 ] && return 0
    [ $z1 -lt $z2 ] && return 1
    return 0
}

#
# Check whether process is still running.
# The first parameter contains the name of the PID file.
# The second parameter is the flag of the need to delete
# the PID file.
# If the second parameter is not zero and not empty,
# then if the process terminates, the corresponding
# PID file will be deleted.
# This function also sets the CHECK_PID variable to zero
# if the process has already exited, or writes the PID
# of the process there if it is still running.
#
check_pid()
{
    local pid_file="$1"
    if [ -r "$pid_file" ]; then
        local pid=$(cat "$pid_file" 2>/dev/null || :)
        if [ -n "$pid" ]; then
            if [ $pid -gt 0 ]; then
                if ps -p $pid >/dev/null 2>&1; then
                    CHECK_PID=$pid
                    return 0
                fi
            fi
        fi
        local remove=${2:-0}
        if [ $remove -ne 0 ]; then
            rm -f "$pid_file" || :
        fi
    fi
    local config="${3:-}"
    if [ -n "$config" -a -f "$config" ]; then
        rm -f "$config" || :
    fi
    CHECK_PID=0
    return 1
}

#
# Checking that the process with the specified PID is still
# running and killing it in this case by sending SIGTERM
# (using the "kill" operation).
# The first parameter contains PID of the process.
# The second and third parameters (both optional) are the names
# of the PID and the configuration files, which should be removed
# after the process ends.
# If the first parameter (PID of the process) is zero, then
# the function immediately deletes the PID and the configuration
# files (if specified), without any additional checks.
#
cleanup_pid()
{
    local pid=$1
    local pid_file="${2:-}"
    local config="${3:-}"

    if [ $pid -gt 0 ]; then
        if ps -p $pid >/dev/null 2>&1; then
            if kill $pid >/dev/null 2>&1; then
                sleep 0.5
                local round=0
                local force=0
                while ps -p $pid >/dev/null 2>&1; do
                    sleep 1
                    round=$(( round+1 ))
                    if [ $round -eq 16 ]; then
                        if [ $force -eq 0 ]; then
                            round=8
                            force=1
                            kill -9 $pid >/dev/null 2>&1 || :
                            sleep 0.5
                        else
                            return 1
                        fi
                    fi
                done
            elif ps -p $pid >/dev/null 2>&1; then
                wsrep_log_warning "Unable to kill PID=$pid ($pid_file)"
                return 1
            fi
        fi
    fi

    [ -n "$pid_file" -a -f "$pid_file" ] && rm -f "$pid_file" || :
    [ -n "$config" -a -f "$config" ] && rm -f "$config" || :

    return 0
}

nproc=""

get_proc()
{
    if [ -z "$nproc" ]; then
        set +e
        if [ "$OS" = 'Linux' ]; then
            nproc=$(grep -c processor /proc/cpuinfo 2>/dev/null)
        elif [ "$OS" = 'Darwin' -o "$OS" = 'FreeBSD' ]; then
            nproc=$(sysctl -n hw.ncpu)
        fi
        set -e
        if [ -z "$nproc" ] || [ $nproc -eq 0 ]; then
            nproc=1
        fi
    fi
}

check_server_ssl_config()
{
    # backward-compatible behavior:
    tcert=$(parse_cnf 'sst' 'tca')
    tcap=$(parse_cnf 'sst' 'tcapath')
    tpem=$(parse_cnf 'sst' 'tcert')
    tkey=$(parse_cnf 'sst' 'tkey')
    # reading new ssl configuration options:
    local tcert2=$(parse_cnf "$encgroups" 'ssl-ca')
    local tcap2=$(parse_cnf "$encgroups" 'ssl-capath')
    local tpem2=$(parse_cnf "$encgroups" 'ssl-cert')
    local tkey2=$(parse_cnf "$encgroups" 'ssl-key')
    # if there are no old options, then we take new ones:
    if [ -z "$tcert" -a -z "$tcap" -a -z "$tpem" -a -z "$tkey" ]; then
        tcert="$tcert2"
        tcap="$tcap2"
        tpem="$tpem2"
        tkey="$tkey2"
    # checking for presence of the new-style SSL configuration:
    elif [ -n "$tcert2" -o -n "$tcap2" -o -n "$tpem2" -o -n "$tkey2" ]; then
        if [ "$tcert" != "$tcert2" -o \
             "$tcap"  != "$tcap2"  -o \
             "$tpem"  != "$tpem2"  -o \
             "$tkey"  != "$tkey2" ]
        then
            wsrep_log_info \
               "new ssl configuration options (ssl-ca[path], ssl-cert" \
               "and ssl-key) are ignored by SST due to presence" \
               "of the tca[path], tcert and/or tkey in the [sst] section"
        fi
    fi
    if [ -n "$tcert" ]; then
        tcert=$(trim_string "$tcert")
        if [ "${tcert%/}" != "$tcert" -o -d "$tcert" ]; then
            tcap="$tcert"
            tcert=""
        fi
    fi
    if [ -n "$tcap" ]; then
        tcap=$(trim_string "$tcap")
    fi
}
