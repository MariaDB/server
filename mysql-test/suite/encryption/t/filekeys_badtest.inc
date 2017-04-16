-- source include/not_embedded.inc
-- source include/have_innodb.inc
-- source filekeys_plugin.inc

--eval call mtr.add_suppression("$SEARCH_PATTERN")
call mtr.add_suppression("Plugin 'file_key_management' init function returned error");
call mtr.add_suppression("Plugin 'file_key_management' registration.*failed");

--let SEARCH_FILE= $MYSQLTEST_VARDIR/log/mysqld.1.err
--source include/search_pattern_in_file.inc

--error ER_CANT_CREATE_TABLE
create table t1(c1 bigint not null, b char(200))  engine=innodb encrypted=yes encryption_key_id=1;

select plugin_status from information_schema.plugins
       where plugin_name = 'file_key_management';

