###########################################
#
# This is a stub of the include file cleanup_engine.inc which 
# should be placed in storage/<engine>/mysql-test/storage_engine folder.
#
################################
#
# Here you can add whatever is needed to cleanup 
# in case your define_engine.inc created any artefacts,
# e.g. an additional schema and/or tables.

--let $datadir= `SELECT @@datadir`

--error 0,1
--file_exists $datadir/\#rocksdb/*
if (!$mysql_errno)
{
  --enable_reconnect
  --exec echo "wait" > $MYSQLTEST_VARDIR/tmp/mysqld.1.expect
  --shutdown_server
  --source include/wait_until_disconnected.inc
  --rmdir $datadir/\#rocksdb
  --exec echo "restart" > $MYSQLTEST_VARDIR/tmp/mysqld.1.expect
  --source include/wait_until_connected_again.inc
}
