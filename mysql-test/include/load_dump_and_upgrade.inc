--source include/have_innodb.inc

# ==== Usage ====
#
# [--let $old_version= 10.2]
# --source include/load_dump_and_upgrade.inc

# Dumps stored in std_data as mysql_database_<old_version>.dump
# were pre-created by running mysqldump for `mysql` database
# on a freshly boostrapped instance of the corresponding version.
# On 10.4 the local root was additionally given an empty password,
# to avoid using unix_socket.

# Backup the existing mysql schema, to restore it later
--exec $MYSQL_DUMP mysql > $MYSQL_TMP_DIR/mysql_database_backup

--echo #
--echo # Upgrade from version $old_version
--echo #

# Workaround for MDEV-22127 (can't import database dump created before 10.4)
# The most universal workaround suggested in the item is running
#   DROP TABLE IF EXISTS `mysql`.`global_priv`;
#   DROP VIEW IF EXISTS `mysql`.`user`;
# before the upgrade. For this test, it is better than creating the dumps
# with --add-drop-database, because
# a) it won't require re-creating the dump when the bug is fixed;
# b) it won't hide possible unrelated problems;
# c) add-drop-database doesn't work with logging into tables enabled (MDEV-4875 and more)

--exec $MYSQL -e "DROP TABLE IF EXISTS mysql.global_priv; DROP VIEW IF EXISTS mysql.user"

--echo # Loading dump of $old_version mysql schema
--exec $MYSQL mysql < $MYSQLTEST_VARDIR/std_data/mysql_database_$old_version.dump

--echo # Running mysql_upgrade
--exec $MYSQL_UPGRADE --verbose > $MYSQL_TMP_DIR/upgrade.log

--echo # Checking that mysql.user is accessible and returns some data
SELECT COUNT(*) > 0 AS `mysql.user has data` FROM mysql.user;

# Restore the backup of mysql schema
--exec $MYSQL mysql < $MYSQL_TMP_DIR/mysql_database_backup

--let $ddir= `select @@datadir`

# It will fail if the file doesn't exist, which is good,
# which is an extra check that it was written
--remove_file $ddir/mysql_upgrade_info
