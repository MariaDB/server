#!/bin/bash

set -e

binlog_line=($(grep -o "Last binlog file position [0-9]*, file name .*\.[0-9]*" ${MYSQLTEST_VARDIR}/log/mysqld.2.err | tail -1))
binlog_pos=${binlog_line[4]%?}
binlog_file=${binlog_line[7]}

sql="show gtid_executed in '$binlog_file' from $binlog_pos"
result=($($MYSQL --defaults-group-suffix=.1 -e "$sql"))
gtid_executed=${result[1]}

sql="reset master;"
sql="$sql reset slave;"
sql="$sql change master to master_host='127.0.0.1', master_port=${MASTER_MYPORT}, master_user='root', master_auto_position=1, master_connect_retry=1;"
sql="$sql set global gtid_purged='$gtid_executed';"
sql="$sql start slave;"
sql="$sql stop slave;"
sql="$sql change master to master_auto_position=0;"
sql="$sql start slave;"
$MYSQL --defaults-group-suffix=.2 -e "$sql"
echo "$sql" > ${MYSQL_TMP_DIR}/gtid_stmt
