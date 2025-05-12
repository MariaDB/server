#!/bin/bash
#
### BEGIN INIT INFO
# Provides:          mariadb
# Required-Start:    $remote_fs $syslog
# Required-Stop:     $remote_fs $syslog
# Should-Start:      $network $named $time
# Should-Stop:       $network $named $time
# Default-Start:     2 3 4 5
# Default-Stop:      0 1 6
# Short-Description: Start and stop the MariaDB database server daemon
# Description:       Controls the main MariaDB database server daemon "mariadbd"
#                    and its wrapper script "mariadbd-safe".
### END INIT INFO
#
set -e
set -u
${DEBIAN_SCRIPT_DEBUG:+ set -v -x}

test -x /usr/sbin/mariadbd || exit 0

# shellcheck source=/dev/null
. /lib/lsb/init-functions

SELF="$(cd "$(dirname "$0")"; pwd -P)/$(basename "$0")"

if [ -f /usr/bin/mariadb-admin ]
then
  MYADMIN="/usr/bin/mariadb-admin --defaults-file=/etc/mysql/debian.cnf"
elif [ -f /usr/bin/mysqladmin ]
then
  MYADMIN="/usr/bin/mysqladmin --defaults-file=/etc/mysql/debian.cnf"
else
  log_failure_msg "Command mariadb-admin/mysqladmin not found! This SysV init script depends on it."
  exit 1
fi

if [ ! -x /usr/bin/mariadbd-safe ]
then
  log_failure_msg "/usr/bin/mariadbd-safe not found or executable! This SysV init script depends on it."
  exit 1
fi

# priority can be overridden and "-s" adds output to stderr
ERR_LOGGER="logger -p daemon.err -t /etc/init.d/mariadb -i"

if [ -f /etc/default/mysql ]
then
  # shellcheck source=/dev/null
  . /etc/default/mysql
fi

# Also source default/mariadb in case the installation was upgraded from
# packages originally installed from MariaDB.org repositories, which have
# had support for reading /etc/default/mariadb since March 2016.
if [ -f /etc/default/mariadb ]
then
  # shellcheck source=/dev/null
  . /etc/default/mariadb
fi

# Safeguard (relative paths, core dumps..)
cd /
umask 077

# mysqladmin likes to read /root/.my.cnf. This is usually not what I want
# as many admins e.g. only store a password without a username there and
# so break my scripts.
export HOME=/etc/mysql/

## Fetch a particular option from mysql's invocation.
#
# Usage: void mariadbd_get_param option
mariadbd_get_param() {
  /usr/sbin/mariadbd --print-defaults \
    | tr " " "\n" \
    | grep -- "--$1" \
    | tail -n 1 \
    | cut -d= -f2
}

## Do some sanity checks before even trying to start mariadbd.
sanity_checks() {
  # check for config file
  if [ ! -r /etc/mysql/my.cnf ]
  then
    log_warning_msg "$0: WARNING: /etc/mysql/my.cnf cannot be read. See README.Debian.gz"
    echo                "WARNING: /etc/mysql/my.cnf cannot be read. See README.Debian.gz" | $ERR_LOGGER
  fi

  # check for diskspace shortage
  datadir="$(mariadbd_get_param datadir)"

  # If datadir location is not customized in configuration
  # then it's not printed with /usr/sbin/mariadbd --print-defaults
  # and this should fall back to a sane default value
  if [ -z "$datadir" ]
  then
    datadir="/var/lib/mysql"
  fi

  # Verify the datadir location exists
  if [ ! -d "$datadir" ] && [ ! -L "$datadir" ]
  then
    log_failure_msg "$0: ERROR: Can't locate MariaDB data location at $datadir"
    echo                "ERROR: Can't locate MariaDB data location at $datadir" | $ERR_LOGGER
    exit 1
  fi

  # As preset blocksize of GNU df is 1024 then available bytes is $df_available_blocks * 1024
  # 4096 blocks is then lower than 4 MB
  df_available_blocks="$(LC_ALL=C BLOCKSIZE='' df --output=avail "$datadir" | tail -n 1)"
  if [ "$df_available_blocks" -lt "4096" ]
  then
    log_failure_msg "$0: ERROR: The partition with $datadir is too full!"
    echo                "ERROR: The partition with $datadir is too full!" | $ERR_LOGGER
    exit 1
  fi
}

## Checks if there is a server running and if so if it is accessible.
#
# check_alive insists on a pingable server
# check_dead also fails if there is a lost mariadbd in the process list
#
# Usage: boolean mariadbd_status [check_alive|check_dead] [warn|nowarn]
mariadbd_status () {
  ping_output="$($MYADMIN ping 2>&1)"
  # The whole mariadbd_status function should be rewritten in clean shell script,
  # so ignore minor Shellcheck nag for now as fixing it would be half of the
  # rewrite
  # shellcheck disable=SC2181
  ping_alive="$(( ! $? ))"

  ps_alive=0
  pidfile="$(mariadbd_get_param pid-file)"
  if [ -f "$pidfile" ] && ps "$(cat "$pidfile")" >/dev/null 2>&1
  then
    ps_alive=1
  fi

  # Using '-a' is unstandard, but it works and might be needed for the grouping
  # of the if-else, so keep it and just ignore in Shellcheck
  # shellcheck disable=SC2166
  if [ "$1" = "check_alive"  -a  $ping_alive = 1 ] ||
     [ "$1" = "check_dead"   -a  $ping_alive = 0  -a  $ps_alive = 0 ]
  then
    return 0 # EXIT_SUCCESS
  else
    if [ "$2" = "warn" ]
    then
      echo -e "$ps_alive processes alive and '$MYADMIN ping' resulted in\n$ping_output\n" | $ERR_LOGGER -p daemon.debug
    fi
  return 1 # EXIT_FAILURE
  fi
}

#
# main()
#

case "${1:-''}" in

  'start')
  sanity_checks;
  # Start daemon
  log_daemon_msg "Starting MariaDB database server" "mariadbd"
  if mariadbd_status check_alive nowarn
  then
   log_progress_msg "already running"
   log_end_msg 0
  else
    # Could be removed during boot
    test -e /run/mysqld || install -m 755 -o mysql -g root -d /run/mysqld

    # Start MariaDB!
    /usr/bin/mariadbd-safe "${@:2}" 2>&1 >/dev/null | $ERR_LOGGER &

    # Make sure that there is some default
    # 30 seconds is fine default for starting
    # maximum is one hour if there is gigantic
    # database
    MARIADB_STARTUP_TIMEOUT=${MYSQLD_STARTUP_TIMEOUT:-30}

    for i in {1..3600}
    do
      if [ "${i}" -gt "${MARIADB_STARTUP_TIMEOUT}" ]
      then
        break
      fi
      sleep 1
      if mariadbd_status check_alive nowarn
      then
        break
      fi
      log_progress_msg "."
    done
    if mariadbd_status check_alive warn
    then
      log_end_msg 0
      # Now start mysqlcheck or whatever the admin wants.
      output=$(/etc/mysql/debian-start)
      if [ -n "$output" ]
      then
        log_action_msg "$output"
      fi
    else
      # Try one more time but save error log separately, then spit it out
      # before logging ends and init script execution ends.
      if pgrep -ax mariadbd > /dev/null
      then
        echo "ERROR: The mariadbd process is running but not responding:"
        # shellcheck disable=SC2009
        # Show the mariadbd process and it's parent and next line (if there is a child process)
        ps faxu | grep mariadbd -C 1
      else
        ERROR_LOG_FILE="$(mktemp).err"
        echo # ensure newline
        timeout --kill-after=20 10 /usr/bin/mariadbd-safe "${@:2}" --log-error="$ERROR_LOG_FILE"
        echo "Running '/etc/init.d/mariadb start' failed with error log:"
        cat "$ERROR_LOG_FILE"
      fi

      log_end_msg 1
      log_failure_msg "Please take a look at the syslog"
    fi
  fi
  ;;

  'stop')
  # * As a passwordless mysqladmin (e.g. via ~/.my.cnf) must be possible
  # at least for cron, we can rely on it here, too. (although we have
  # to specify it explicit as e.g. sudo environments points to the normal
  # users home and not /root)
  log_daemon_msg "Stopping MariaDB database server" "mariadbd"
  if ! mariadbd_status check_dead nowarn
  then
    set +e
    shutdown_out="$($MYADMIN shutdown 2>&1)"
    r=$?
    set -e
    if [ "$r" -ne 0 ]
    then
      log_end_msg 1
      [ "$VERBOSE" != "no" ] && log_failure_msg "Error: $shutdown_out"
      log_daemon_msg "Killing MariaDB database server by signal" "mariadbd"
      killall -15 mariadbd
      server_down=
      for _ in {1..600}
      do
        sleep 1
        if mariadbd_status check_dead nowarn
        then
          server_down=1
          break
        fi
      done
      if test -z "$server_down"
      then
        killall -9 mariadbd
      fi
    fi
  fi

  if ! mariadbd_status check_dead warn
  then
    log_end_msg 1
    log_failure_msg "Please stop MariaDB manually and read /usr/share/doc/mariadb-server/README.Debian.gz!"
    exit 1
  else
    log_end_msg 0
  fi
  ;;

  'restart')
  set +e; $SELF stop; set -e
  shift
  $SELF start "${@}"
  ;;

  'reload'|'force-reload')
  log_daemon_msg "Reloading MariaDB database server" "mariadbd"
  $MYADMIN reload
  log_end_msg 0
  ;;

  'status')
  if mariadbd_status check_alive nowarn
  then
    log_action_msg "$($MYADMIN version)"
  else
    log_action_msg "MariaDB is stopped."
    exit 3
  fi
  ;;

  'bootstrap')
  # Bootstrap the cluster, start the first node
  # that initiates the cluster
  log_daemon_msg "Bootstrapping the cluster" "mariadbd"
  $SELF start "${@:2}" --wsrep-new-cluster
  ;;

  *)
  echo "Usage: $SELF start|stop|restart|reload|force-reload|status"
  exit 1
  ;;
esac
