#!/bin/bash
set -e

# shellcheck source=/dev/null
. /usr/share/debconf/confmodule

if [ -n "$DEBIAN_SCRIPT_DEBUG" ]
then
  set -v -x
  DEBIAN_SCRIPT_TRACE=1
fi

${DEBIAN_SCRIPT_TRACE:+ echo "#42#DEBUG# RUNNING $0 $*" 1>&2 }

#
# - Purge logs and data only if they are ours (#307473)
# - Remove the mysql user only after all his owned files are purged.
# - Cleanup the initscripts only if this was the last provider of them
#
if [ "$1" = "purge" ] && [ -f "/var/lib/mysql/debian-__MARIADB_MAJOR_VER__.flag" ]
then
  # we remove the mysql user only after all his owned files are purged
  rm -f /var/log/mysql.{log,err}{,.0,.[1234567].gz}
  rm -rf /var/log/mysql

  db_input high "mariadb-server/postrm_remove_databases" || true
  db_go || true
  db_get "mariadb-server/postrm_remove_databases" || true
  if [ "$RET" = "true" ]
  then
    # never remove the debian.cnf when the databases are still existing
    # else we ran into big trouble on the next install!
    rm -f /etc/mysql/debian.cnf
    # Remove all contents from /var/lib/mysql except if it's a
    # directory with file system data. See #829491 for details and
    # #608938 for potential mysql-server leftovers which erroneously
    # had been renamed.
    # Attempt removal only if the directory hasn't already been removed
    # by dpkg to avoid failing on "No such file or directory" errors.
    if [ -d /var/lib/mysql ]
    then
      find /var/lib/mysql -mindepth 1 \
        -not -path '*/lost+found/*'     -not -name 'lost+found' \
        -not -path '*/lost@002bfound/*' -not -name 'lost@002bfound' \
        -delete

      # "|| true" still needed as rmdir still exits with non-zero if
      # /var/lib/mysql is a mount point
      rmdir --ignore-fail-on-non-empty /var/lib/mysql || true
    fi
    rm -rf /run/mysqld # this directory is created by the init script, don't leave behind
    userdel mysql || true
  fi

fi

#DEBHELPER#

# Modified dh_systemd_start snippet that's not added automatically
if [ -d /run/systemd/system ]
then
  systemctl --system daemon-reload >/dev/null || true
fi
