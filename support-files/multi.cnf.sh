#
# This adds example of how to setup multiple running MariaDB servers. It is
# by default commented out, but it can be used as an example and help people
# start with new configuration.
#

[mysqld_multi]
mysqld     = @bindir@/mysqld_safe
mysqladmin = @bindir@/mysqladmin
log        = @localstatedir@/mysqld_multi.log
# user       = multi_admin
# password   = secret

# If you want to use mysqld_multi uncomment 1 or more mysqld sections
# below or add your own ones.

# WARNING
# --------
# If you uncomment mysqld1 than make absolutely sure, that database mysql,
# configured above, is not started.  This may result in corrupted data!
# [mysqld1]
# port       = @MYSQL_TCP_PORT@
# datadir    = /var/lib/mysql
# pid-file   = /var/lib/mysql/mysqld.pid
# socket     = /var/lib/mysql/mysql.sock
# user       = mysql

# [mysqld2]
# port       = 3307
# datadir    = /var/lib/mysql-databases/mysqld2
# pid-file   = /var/lib/mysql-databases/mysqld2/mysql.pid
# socket     = /var/lib/mysql-databases/mysqld2/mysql.sock
# user       = mysql

# [mysqld3]
# port       = 3308
# datadir    = /var/lib/mysql-databases/mysqld3
# pid-file   = /var/lib/mysql-databases/mysqld3/mysql.pid
# socket     = /var/lib/mysql-databases/mysqld3/mysql.sock
# user       = mysql

# [mysqld6]
# port       = 3309
# datadir    = /var/lib/mysql-databases/mysqld6
# pid-file   = /var/lib/mysql-databases/mysqld6/mysql.pid
# socket     = /var/lib/mysql-databases/mysqld6/mysql.sock
# user       = mysql

