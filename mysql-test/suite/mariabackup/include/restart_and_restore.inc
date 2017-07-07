let $_datadir= `SELECT @@datadir`;
echo # shutdown server;
--source include/shutdown_mysqld.inc
echo # remove datadir;
rmdir $_datadir;
echo # xtrabackup move back;
exec $XTRABACKUP --defaults-file=$MYSQLTEST_VARDIR/my.cnf --copy-back --datadir=$_datadir --target-dir=$targetdir --parallel=2 --throttle=1;
echo # restart server;
--source include/start_mysqld.inc
