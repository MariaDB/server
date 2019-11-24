#!/bin/sh
#
# A simple startup script for mysqld_multi by Tim Smith and Jani Tolonen.
# This script assumes that my.cnf file exists either in /etc/my.cnf or
# /root/.my.cnf and has groups [mysqld_multi] and [mysqldN]. See the
# mysqld_multi documentation for detailed instructions.
#
# This script can be used as /etc/init.d/mysql.server
#
# Comments to support chkconfig on RedHat Linux
# chkconfig: 2345 64 36
# description: A very fast and reliable SQL database engine.
#
# Version 1.0
#

### BEGIN INIT INFO
# Provides:          mysqld_multi
# Required-Start: $local_fs $network $remote_fs
# Should-Start: ypbind nscd ldap ntpd xntpd
# Required-Stop: $local_fs $network $remote_fs
# Default-Start:  2 3 4 5
# Default-Stop: 0 1 6
# Short-Description: Start and stop multiple mysql database server daemon instances
# Description:       Controls multiple MariaDB database server daemon instances
### END INIT INFO

PATH=/usr/local/sbin:/usr/local/bin:/sbin:/bin:/usr/sbin:/usr/bin
NAME=mysqld_multi
DESC=mysqld_multi

basedir=/usr
bindir=/usr/bin

if test -x $bindir/mysqld_multi
then
  mysqld_multi="$bindir/mysqld_multi";
else
  echo "Can't execute $bindir/mysqld_multi from dir $basedir";
  exit;
fi

case "$1" in
    'start' )
        "$mysqld_multi" start $2
        ;;
    'stop' )
        "$mysqld_multi" stop $2
        ;;
    'report' )
        "$mysqld_multi" report $2
        ;;
    'restart' )
        "$mysqld_multi" stop $2
        "$mysqld_multi" start $2
        ;;
    *)
        echo "Usage: $0 {start|stop|report|restart}" >&2
        ;;
esac
