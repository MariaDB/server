--echo Shutting down server ...
--let $datadir=`select @@datadir`
--let $innodb_home=`select @@innodb_data_home_dir`
--let $innodb_group=`select @@innodb_log_group_home_dir`
--let $innodb_undo=`select @@innodb_undo_directory`
if ($innodb_home == './') {
--let $innodb_home=$datadir
}
if (! $innodb_home) {
--let $innodb_home=$datadir
}
if ($innodb_group == './') {
--let $innodb_group=$datadir
}
if (! $innodb_group) {
--let $innodb_group=$datadir
}
if ($innodb_undo == './') {
--let $innodb_undo=$datadir
}
if (! $innodb_undo) {
--let $innodb_undo=$datadir
}
--source include/shutdown_mysqld.inc

--connection node_1
--let $wait_condition = SELECT VARIABLE_VALUE = 1 FROM INFORMATION_SCHEMA.GLOBAL_STATUS WHERE VARIABLE_NAME = 'wsrep_cluster_size'
--source include/wait_condition.inc

--echo Cleaning var directory ...
if ($innodb_home != $datadir) {
--remove_files_wildcard $innodb_home
}
if ($innodb_group != $datadir) {
--remove_files_wildcard $innodb_group
}
if ($innodb_undo != $datadir) {
--remove_files_wildcard $innodb_undo
}
--remove_files_wildcard $datadir/mtr
--remove_files_wildcard $datadir/mysql
--remove_files_wildcard $datadir/performance_schema
--remove_files_wildcard $datadir/sys
--remove_files_wildcard $datadir/test
--remove_files_wildcard $datadir
