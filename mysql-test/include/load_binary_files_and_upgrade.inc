# ==== Usage ====
#
# [--let $old_version= xx.x]
# --source include/load_binary_files_and_upgrade.inc
let $MYSQLD_DATADIR= `select @@datadir`;

# mysql.columns_priv
--remove_file $MYSQLD_DATADIR/mysql/columns_priv.frm
--remove_file $MYSQLD_DATADIR/mysql/columns_priv.MAD
--remove_file $MYSQLD_DATADIR/mysql/columns_priv.MAI
--copy_file std_data/binary_upgrade/$old_version/columns_priv.frm $MYSQLD_DATADIR/mysql/columns_priv.frm
--copy_file std_data/binary_upgrade/$old_version/columns_priv.MAD $MYSQLD_DATADIR/mysql/columns_priv.MAD
--copy_file std_data/binary_upgrade/$old_version/columns_priv.MAI $MYSQLD_DATADIR/mysql/columns_priv.MAI

# mysql.db
--remove_file $MYSQLD_DATADIR/mysql/db.frm
--remove_file $MYSQLD_DATADIR/mysql/db.MAD
--remove_file $MYSQLD_DATADIR/mysql/db.MAI
--copy_file std_data/binary_upgrade/$old_version/db.frm $MYSQLD_DATADIR/mysql/db.frm
--copy_file std_data/binary_upgrade/$old_version/db.MAD $MYSQLD_DATADIR/mysql/db.MAD
--copy_file std_data/binary_upgrade/$old_version/db.MAI $MYSQLD_DATADIR/mysql/db.MAI

# mysql.user
--remove_file $MYSQLD_DATADIR/mysql/user.frm
--copy_file std_data/binary_upgrade/$old_version/user.frm $MYSQLD_DATADIR/mysql/user.frm

# more binary files can be added as deemed necessary

--echo # Running mysql_upgrade
--exec $MYSQL_UPGRADE --verbose > $MYSQL_TMP_DIR/upgrade.log

--echo # Checking that mysql.columns_priv is accessible
SELECT * FROM mysql.columns_priv;

--echo # Checking that mysql.db is accessible
SELECT * FROM mysql.db;

--echo # Checking that mysql.user is accessible
SELECT COUNT(*) > 0 as 'mysql.user has data' FROM mysql.user;

# It will fail if the file doesn't exist, which is good,
# which is an extra check that it was written and that the upgrade succeeded
--remove_file $MYSQLD_DATADIR/mariadb_upgrade_info
