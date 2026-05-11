DROP DATABASE IF EXISTS m8_db;
DROP DATABASE IF EXISTS `m8_under_score_db`;

--source include/restore_priv_tables.inc

--remove_file $MYSQLTEST_VARDIR/tmp/m8_import.sql
--remove_file $MYSQLTEST_VARDIR/tmp/m8_import.err
--remove_file $MYSQLTEST_VARDIR/tmp/m8_import.out
