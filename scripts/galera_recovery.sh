#!/bin/sh

# Copyright (c) 2016 MariaDB Corporation
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
# along with this program; if not, write to the Free Software
# Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1335  USA */


# This script is intended to be executed by systemd. It starts mysqld with
# --wsrep-recover to recover from a non-graceful shutdown, determines the
# last stored global transaction ID and echoes it in --wsrep-start-position=XX
# format. The output is then captured and used by systemd to start mysqld.
# If the server was configured to start without wsrep, nothing is echoed.

cmdline_args=$@
user="@MYSQLD_USER@"
print_defaults="@bindir@/my_print_defaults"
log_file=$(mktemp /tmp/wsrep_recovery.XXXXXX)
euid=$(id -u)
recovered_pos=""
skipped=""
start_pos=""
start_pos_opt=""
ret=0
wsrep_on=0

log ()
{
  local msg="$1"
  # Print all messages to stderr as we reserve stdout for printing
  # --wsrep-start-position=XXXX.
  echo "$msg" >&2
}

finish()
{
  rm -f "$log_file"
}

trap finish EXIT

parse_arguments() {
  for arg do
    val=`echo "$arg" | sed -e "s;--[^=]*=;;"`
    case "$arg" in
      --wsrep[-_]on) wsrep_on=1 ;;
      --skip[-_]wsrep[-_]on) wsrep_on=0 ;;
      --wsrep[-_]on=*)
        if echo $val | grep -iq '\(ON\|1\)'; then
          wsrep_on=1
        else
          wsrep_on=0
        fi
        ;;
    esac
  done
}

wsrep_recover_position() {
  # Redirect server's error log to the log file.
  eval @sbindir@/mysqld $cmdline_args --user=$user --wsrep_recover \
    --disable-log-error 2> "$log_file"
  ret=$?
  if [ $ret -ne 0 ]; then
    # Something went wrong, let us also print the error log so that it
    # shows up in systemctl status output as a hint to the user.
    log "WSREP: Failed to start mysqld for wsrep recovery: '`cat $log_file`'"
    exit 1
  fi

  # Parse server's error log for recovered position. The server prints
  # "..skipping position recovery.." if started without wsrep.

  recovered_pos="$(grep 'WSREP: Recovered position:' $log_file)"

  if [ -z "$recovered_pos" ]; then
    skipped="$(grep WSREP $log_file | grep 'skipping position recovery')"
    if [ -z "$skipped" ]; then
      log "WSREP: Failed to recover position: '`cat $log_file`'"
      exit 1
    else
      log "WSREP: Position recovery skipped."
    fi
  else
    start_pos="$(echo $recovered_pos | sed 's/.*WSREP\:\ Recovered\ position://' \
                    | sed 's/^[ \t]*//')"
    log "WSREP: Recovered position $start_pos"
    start_pos_opt="--wsrep_start_position=$start_pos"
  fi
}

# Safety checks
if [ -n "$log_file" -a -f "$log_file" ]; then
  chmod 600 $log_file
else
  log "WSREP: mktemp failed"
fi

parse_arguments `$print_defaults $cmdline_args --loose-verbose --mysqld`

# Perform wsrep position recovery if wsrep_on=1, skip otherwise.
if [ "$wsrep_on" -eq 1 ]; then
  wsrep_recover_position
fi

echo "$start_pos_opt"

