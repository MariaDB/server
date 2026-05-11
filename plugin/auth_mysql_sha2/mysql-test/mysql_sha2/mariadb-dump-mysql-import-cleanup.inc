DROP DATABASE src_db;
DROP DATABASE `src_under_score_db`;

--source include/restore_priv_tables.inc

--remove_file $MYSQLTEST_VARDIR/tmp/src_import.sql
--remove_file $MYSQLTEST_VARDIR/tmp/src_import.err
--remove_file $MYSQLTEST_VARDIR/tmp/src_import.out
