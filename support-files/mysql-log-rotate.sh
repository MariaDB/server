# This logname can be set in /etc/my.cnf
# by setting the variable "log_error"
# in the [mysqld] section as follows:
#
# [mysqld]
# log_error=@LOG_LOCATION@

@LOG_LOCATION@ {
        create 600 mysql mysql
        notifempty
	daily
        rotate 3
        missingok
        compress
    postrotate
	# just if mysqld is really running
        if [ -e @PID_FILE_DIR@/@DAEMON_NO_PREFIX@.pid ]
        then
           # mysqld will flush the logs after recieving a SIGHUP
           kill -1 $(<@PID_FILE_DIR@/@DAEMON_NO_PREFIX@.pid)
        fi
    endscript
}
