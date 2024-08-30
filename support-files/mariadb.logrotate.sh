# This is the MariaDB configuration for the logrotate utility
#
# Note that on most Linux systems logs are written to journald, which has its
# own rotation scheme.
#
# Read https://mariadb.com/kb/en/error-log/ to learn more about logging and
# https://mariadb.com/kb/en/rotating-logs-on-unix-and-linux/ about rotating logs.

@localstatedir@/mysqld.log @localstatedir@/mariadb.log @logdir@/*.log {

  # Depends on a mysql@localhost unix_socket authenticated user with RELOAD privilege
  @su_user@

  # If any of the files listed above is missing, skip them silently without
  # emitting any errors
  missingok

  # If file exists but is empty, don't rotate it
  notifempty

  # Run monthly
  monthly

  # Keep 6 months of logs
  rotate 6

  # If file is growing too big, rotate immediately
  maxsize 500M

  # If file size is too small, don't rotate at all
  minsize 50M

  # Compress logs, as they are text and compression will save a lot of disk space
  compress

  # Don't compress the log immediately to avoid errors about "file size changed while zipping"
  delaycompress

  # Don't run the postrotate script for each file configured in this file, but
  # run it only once if one or more files were rotated
  sharedscripts

  # After each rotation, run this custom script to flush the logs. Note that
  # this assumes that the mariadb-admin command has database access, which it
  # has thanks to the default use of Unix socket authentication for the 'mysql'
  # (or root on Debian) account used everywhere since MariaDB 10.4.
  postrotate
    if test -r /etc/mysql/debian.cnf
    then
      EXTRAPARAM='--defaults-file=/etc/mysql/debian.cnf'
    fi

    if test -x @bindir@/mariadb-admin
    then
      @bindir@/mariadb-admin $EXTRAPARAM --local flush-error-log \
        flush-engine-log flush-general-log flush-slow-log
    fi
  endscript
}
