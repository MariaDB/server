--source include/not_embedded.inc

if (`select count(*) = 0 from information_schema.plugins where plugin_name = 'caching_sha2_password'`)
{
  --skip Needs caching_sha2_password plugin
}

SET NAMES utf8mb4;

--source include/backup_priv_tables.inc

# Schema referenced by table-/column-/routine-level grants in the dump.
CREATE DATABASE src_db;
CREATE TABLE src_db.tab (col_a INT, col_underscore_b INT, plain INT);
CREATE TABLE src_db.tab2 (id INT PRIMARY KEY, payload TEXT);
USE src_db;
DELIMITER $$;
CREATE PROCEDURE proc_one(IN x INT) BEGIN SELECT x; END$$
CREATE FUNCTION  func_one(x INT) RETURNS INT DETERMINISTIC BEGIN RETURN x*2; END$$
DELIMITER ;$$
USE test;
CREATE DATABASE `src_under_score_db`;

# auth_mysql_sha2 is loaded via .opt - silence its load-time warnings.
CALL mtr.add_suppression('failed to read private_key.pem: 2 "No such file or directory"');
CALL mtr.add_suppression("Plugin 'caching_sha2_password' is of maturity level gamma");

--source include/create_dump_importer.inc

# --force goes on past the expected ER 1396 for root@localhost, which
# already exists on MariaDB; any other error shows up as FOUND.
# The appended query shows whether the dump restores the session sql_mode.
--copy_file $MYSQLTEST_VARDIR/std_data/mysql_users_dump.sql $MYSQLTEST_VARDIR/tmp/src_import.sql
--append_file $MYSQLTEST_VARDIR/tmp/src_import.sql
SELECT IF(@@sql_mode = @@global.sql_mode, 'sql_mode restored', 'sql_mode LEAKED');
EOF
--exec $MYSQL --user=test_restore --password=r --force -N < $MYSQLTEST_VARDIR/tmp/src_import.sql > $MYSQLTEST_VARDIR/tmp/src_import.out 2> $MYSQLTEST_VARDIR/tmp/src_import.err

--let SEARCH_FILE=$MYSQLTEST_VARDIR/tmp/src_import.err
--let SEARCH_PATTERN=(?:\A|\n)ERROR (?!1396 )\d
--source include/search_pattern_in_file.inc
--let SEARCH_PATTERN=(?:\A|\n)ERROR 1396 (?![^\n]*'root'@'localhost')
--source include/search_pattern_in_file.inc
--let SEARCH_FILE=$MYSQLTEST_VARDIR/tmp/src_import.out
--let SEARCH_PATTERN=sql_mode restored
--let SEARCH_ABORT=NOT FOUND
--source include/search_pattern_in_file.inc
--let SEARCH_ABORT=

--echo # The import role is gone and the restoring user is not left as admin:
SELECT User, Role FROM mysql.roles_mapping
  WHERE User='test_restore' OR Role='mariadb_dump_import_role';
--source include/drop_dump_importer.inc
