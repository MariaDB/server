#
# This is failing test case from the MDEV-19157
#  We will make some inserts/updates in the master which are conflicting
#  And gtid_slave_pos is non transactional
#

# Create some conflicting  events on master
connection master;
CREATE TABLE t1(a INT NOT NULL AUTO_INCREMENT PRIMARY KEY) engine=innodb;
INSERT INTO t1 VALUES (1),(2),(3),(4);
--disable_query_log
--let $count=5
while ($count)
{
update t1 set a=a+5 where a=1;
update t1 set a=a+5 where a=2;
update t1 set a=a+5 where a=3;
update t1 set a=a+5 where a=4;
update t1 set a=a-5 where a=5;
update t1 set a=a-5 where a=6;
--dec $count
}
--enable_query_log
select * from t1 order by a;
drop table t1;
--sync_slave_with_master
connection master;
