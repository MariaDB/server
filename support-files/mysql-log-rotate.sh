# This logname can be set in /etc/my.cnf
# by setting the variable "log-error"
# in the [mysqld_safe] section as follows:
#
# [mysqld_safe]
# log-error=@localstatedir@/mysqld.log
#
# If the root user has a password you have to create a
# /root/.my.cnf configuration file with the following
# content:
#
# [mysqladmin]
# password = <secret> 
# user= root
#
# where "<secret>" is the password. 
#
# ATTENTION: This /root/.my.cnf should be readable ONLY
# for root !

@localstatedir@/mysqld.log {
        # create 600 mysql mysql
        notifempty
	daily
        rotate 3
        missingok
        compress
    postrotate
	# just if mysqld is really running
	if test -x @bindir@/mysqladmin && \
	   @bindir@/mysqladmin ping &>/dev/null
	then
	   @bindir@/mysqladmin flush-logs
	fi
    endscript
}
