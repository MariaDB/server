source include/have_rocksdb.inc;
source include/master-slave.inc;
-- let $uuid = `select @@server_uuid;`

--exec echo > $MYSQLTEST_VARDIR/log/mysqld.1.err

connection master;
--disable_warnings
drop table if exists x;
--enable_warnings

connection master;

select @@binlog_format;

create table x (id int primary key, value int, value2 int, index(value)) engine=rocksdb;
insert into x values (1,1,1);
insert into x values (2,1,1);
insert into x values (3,1,1);
insert into x values (4,1,1);
insert into x values (5,1,1);
-- replace_result $uuid uuid
select @@global.gtid_executed;

sync_slave_with_master;
connection slave;
--let slave_data_dir= query_get_value(SELECT @@DATADIR, @@DATADIR, 1)
--let slave_pid_file= query_get_value(SELECT @@pid_file, @@pid_file, 1)
--disable_query_log
select "--- slave state before crash ---" as "";
--enable_query_log
select * from x;
-- replace_result $uuid uuid
select @@global.gtid_executed;
-- replace_result $uuid uuid
select * from mysql.slave_gtid_info;

--exec echo "wait" > $MYSQLTEST_VARDIR/tmp/mysqld.2.expect

--write_file $MYSQL_TMP_DIR/truncate_tail_wal.sh
#!/bin/bash

F=`ls -t $slave_data_dir/\#rocksdb/*.log | head -n 1`
SIZE=`stat -c %s $F`
NEW_SIZE=`expr $SIZE - 30`
truncate -s $NEW_SIZE $F
rc=$?
if [[ $rc != 0 ]]; then
  exit 1
fi

kill -9 `head -1 $slave_pid_file`

exit 0
EOF
--chmod 0755 $MYSQL_TMP_DIR/truncate_tail_wal.sh
--exec $MYSQL_TMP_DIR/truncate_tail_wal.sh

--let $rpl_skip_start_slave= 1
--source include/rpl_start_server.inc
--disable_query_log
select "--- slave state after crash recovery, slave stop, one transaction recovered---" as "";
--enable_query_log
connection slave;
--exec python suite/rocksdb/t/check_log_for_xa.py $MYSQLTEST_VARDIR/log/mysqld.2.err commit,prepare,rollback
select * from x;
-- replace_result $uuid uuid
select @@global.gtid_executed;
-- replace_result $uuid uuid
select * from mysql.slave_gtid_info;

--disable_query_log
select "--- slave state after restart, slave start ---" as "";
--enable_query_log
--source include/start_slave.inc
connection master;
sync_slave_with_master;
connection slave;
select * from x;
-- replace_result $uuid uuid
select @@global.gtid_executed;
-- replace_result $uuid uuid
select * from mysql.slave_gtid_info;

connection master;
insert into x values (6,1,1);

sync_slave_with_master;
connection slave;
select * from x;
-- replace_result $uuid uuid
select @@global.gtid_executed;
-- replace_result $uuid uuid
select * from mysql.slave_gtid_info;

connection master;
insert into x values (7,1,1);
insert into x values (8,1,1);
insert into x values (9,1,1);
insert into x values (10,1,1);
insert into x values (11,1,1);
insert into x values (12,1,1);
select * from x;
-- replace_result $uuid uuid
select @@global.gtid_executed;
sync_slave_with_master;

connection slave;

# Corrupting WAL. MyRocks does point in time recovery with wal_recovery_mode=2.
# It loses some data but can resync after restarting slave.

--exec echo "wait" > $MYSQLTEST_VARDIR/tmp/mysqld.2.expect

--write_file $MYSQL_TMP_DIR/corrupt_wal.sh
#!/bin/bash

# expected to be around 950 bytes
F=`ls -t $slave_data_dir/\#rocksdb/*.log | head -n 1`
SIZE=`stat -c %s $F`
OFFSET=$(( $SIZE-500 ))
dd bs=1 if=/dev/zero of=$F count=100 seek=$OFFSET conv=notrunc

kill -9 `head -1 $slave_pid_file`

exit 0
EOF
--chmod 0755 $MYSQL_TMP_DIR/corrupt_wal.sh
--exec $MYSQL_TMP_DIR/corrupt_wal.sh

--let $rpl_skip_start_slave= 1
--source include/rpl_start_server.inc
--disable_query_log
select "--- slave state after crash recovery, slave stop, WAL was corrupted, point in time recovery with wal_recovery_mode=2 ---" as "";
--enable_query_log
select * from x;
--source include/start_slave.inc
connection master;
sync_slave_with_master;
connection slave;
select * from x;
-- replace_result $uuid uuid
select @@global.gtid_executed;
-- replace_result $uuid uuid
select * from mysql.slave_gtid_info;

connection master;
drop table x;


--remove_file $MYSQL_TMP_DIR/truncate_tail_wal.sh
--remove_file $MYSQL_TMP_DIR/corrupt_wal.sh
--source include/rpl_end.inc

