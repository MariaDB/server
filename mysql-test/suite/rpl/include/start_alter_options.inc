# This test will test all the option related to the Alter Table command
# NOTE not all alter statements will follow alter_algorithm since for some statements
# copy is only option
# parameters
# $alter_algorithm   {DEFAULT|INPLACE|COPY|NOCOPY|INSTANT}
# $show_binlog
#
--let $force_needed= force ,
# Error that is caused by a particular ALTER's option combination
--let $alter_error = 0

--connection slave
stop slave;
--let $gtid_strict_mode= `select @@gtid_strict_mode`
--let $slave_parallel_threads= `select @@slave_parallel_threads`
--let $slave_parallel_mode= `select @@slave_parallel_mode`
change master to master_use_gtid= current_pos;
SET GLOBAL slave_parallel_threads=4;
set global slave_parallel_mode=optimistic;
set global gtid_strict_mode=1;
start slave;
--connection master
set binlog_alter_two_phase=true;
create table t1(a int , b int) engine=innodb;
create table a1(a int , b int) engine=myisam;
create temporary table tmp_tbl(a int, b int) engine=innodb;

#  These are grammer rules for ALTER TABLE we will got through all alter table
#  rules in this test.
#  | ADD [COLUMN] [IF NOT EXISTS] col_name column_definition
#        [FIRST | AFTER col_name ]
#  | ADD [COLUMN] [IF NOT EXISTS] (col_name column_definition,...)
if (`select '$alter_algorithm' = 'instant' or  '$alter_algorithm' = 'nocopy'`)
{
 --let $alter_error=ER_ALTER_OPERATION_NOT_SUPPORTED
}
--error 0,$alter_error
--eval alter table t1 add column if not exists c int , $force_needed algorithm=$alter_algorithm
if (`select '$alter_algorithm' = 'instant' or  '$alter_algorithm' = 'nocopy'`)
{
 --let $alter_error=ER_ALTER_OPERATION_NOT_SUPPORTED
}
--error 0,$alter_error
--eval alter table t1 add column d int first, $force_needed algorithm=$alter_algorithm
if (`select '$alter_algorithm' = 'instant' or  '$alter_algorithm' = 'nocopy'`)
{
 --let $alter_error=ER_BAD_FIELD_ERROR
}
--error 0,$alter_error
--eval alter table t1 add column e int after c, $force_needed algorithm=$alter_algorithm
if (`select '$alter_algorithm' = 'instant' or  '$alter_algorithm' = 'nocopy'`)
{
 --let $alter_error=ER_BAD_FIELD_ERROR
}
--error 0,$alter_error
--eval alter table t1 add column f int after c, $force_needed add column g int first ,add column  h char, algorithm=$alter_algorithm
if (`select '$alter_algorithm' = 'instant' or  '$alter_algorithm' = 'nocopy'`)
{
 --let $alter_error=ER_CANT_DROP_FIELD_OR_KEY
}
--error 0,$alter_error
--eval alter table t1 drop column c, drop column d, drop column e, drop column f, drop column g , drop column h, $force_needed algorithm=$alter_algorithm
if ($alter_algorithm == 'copy')
{
--eval alter table tmp_tbl add column if not exists c int , $force_needed algorithm=$alter_algorithm
--eval alter table tmp_tbl add column d int first, $force_needed algorithm=$alter_algorithm
--eval alter table tmp_tbl add column e int after c, $force_needed algorithm=$alter_algorithm
--eval alter table tmp_tbl add column f int after c, $force_needed add column g int first ,add column  h char, algorithm=$alter_algorithm
--eval alter table tmp_tbl drop column c, drop column d, drop column e, drop column f, drop column g , drop column h, $force_needed algorithm=$alter_algorithm
}
--echo # show binlog and clear status
--sync_slave_with_master
if ($show_binlog)
{
  --source include/show_binlog_events.inc
}
reset master;
--connection master


#
#  | ADD {INDEX|KEY} [IF NOT EXISTS] [index_name]
#        [index_type] (index_col_name,...) [index_option] ...
#  | ADD [CONSTRAINT [symbol]]
#        UNIQUE [INDEX|KEY] [index_name]
#        [index_type] (index_col_name,...) [index_option] ...
#
#  | ADD FULLTEXT [INDEX|KEY] [index_name]
#        (index_col_name,...) [index_option] ...
#  | DROP [COLUMN] [IF EXISTS] col_name [RESTRICT|CASCADE]
#  | DROP PRIMARY KEY
#  | DROP {INDEX|KEY} [IF EXISTS] index_name
#  | DROP FOREIGN KEY [IF EXISTS] fk_symbol
#  | DROP CONSTRAINT [IF EXISTS] constraint_name
if (`select '$alter_algorithm' = 'instant' or  '$alter_algorithm' = 'nocopy'`)
{
 --let $alter_error=ER_ALTER_OPERATION_NOT_SUPPORTED
}
--error 0,$alter_error
--eval alter table t1 add column f int after b, $force_needed add column g int first ,add column  h varchar(100), algorithm=$alter_algorithm
if (`select '$alter_algorithm' = 'instant' or  '$alter_algorithm' = 'nocopy'`)
{
 --let $alter_error=ER_KEY_COLUMN_DOES_NOT_EXIST
}
--error 0,$alter_error
--eval alter table t1 add index if not exists index_1(f), $force_needed algorithm=$alter_algorithm
if (`select '$alter_algorithm' = 'instant' or  '$alter_algorithm' = 'nocopy'`)
{
 --let $alter_error=ER_CANT_DROP_FIELD_OR_KEY
}
--error 0,$alter_error
--eval alter table t1 drop index index_1, $force_needed algorithm=$alter_algorithm
if (`select '$alter_algorithm' = 'instant' or  '$alter_algorithm' = 'nocopy'`)
{
 --let $alter_error=ER_KEY_COLUMN_DOES_NOT_EXIST
}
--error 0,$alter_error
--eval alter table t1 add unique key unique_1(g), $force_needed algorithm=$alter_algorithm
if (`select '$alter_algorithm' = 'instant' or  '$alter_algorithm' = 'nocopy'`)
{
 --let $alter_error=ER_CANT_DROP_FIELD_OR_KEY
}
--error 0,$alter_error
--eval alter table t1 drop index unique_1, $force_needed algorithm=$alter_algorithm
if (`select '$alter_algorithm' = 'instant' or  '$alter_algorithm' = 'nocopy'`)
{
 --let $alter_error=ER_KEY_COLUMN_DOES_NOT_EXIST
}
--error 0,$alter_error
--eval alter table t1 add fulltext  key f_1(h), $force_needed algorithm=$alter_algorithm
if (`select '$alter_algorithm' = 'instant' or  '$alter_algorithm' = 'nocopy'`)
{
 --let $alter_error=ER_CANT_DROP_FIELD_OR_KEY
}
--error 0,$alter_error
--eval alter table t1  drop column f, drop column g , $force_needed algorithm=$alter_algorithm
if ($alter_algorithm == 'copy')
{
--eval alter table tmp_tbl add column f int after b, $force_needed add column g int first ,add column  h varchar(100), algorithm=$alter_algorithm
if (`select '$alter_algorithm' = 'instant' or  '$alter_algorithm' = 'nocopy'`)
{
 --let $alter_error=ER_KEY_COLUMN_DOES_NOT_EXIST
}
--error 0,$alter_error
--eval alter table tmp_tbl add index if not exists index_1(f), $force_needed algorithm=$alter_algorithm
--eval alter table tmp_tbl drop index index_1, $force_needed algorithm=$alter_algorithm
--eval alter table tmp_tbl add unique key unique_1(g), $force_needed algorithm=$alter_algorithm
--eval alter table tmp_tbl drop index unique_1, $force_needed algorithm=$alter_algorithm
--eval alter table tmp_tbl drop column f, drop column g , $force_needed algorithm=$alter_algorithm
}
#  | ADD [CONSTRAINT [symbol]] PRIMARY KEY
#        [index_type] (index_col_name,...) [index_option] ...
# primary key changes cant use inplace algorithm
if (`select '$alter_algorithm' = 'instant' or  '$alter_algorithm' = 'nocopy'`)
{
 --let $alter_error=ER_KEY_COLUMN_DOES_NOT_EXIST
}
--error 0,$alter_error
--eval alter table t1 add primary key(h), $force_needed algorithm=copy
if (`select '$alter_algorithm' = 'instant' or  '$alter_algorithm' = 'nocopy'`)
{
 --let $alter_error=ER_CANT_DROP_FIELD_OR_KEY
}
--error 0,$alter_error
--eval alter table t1 drop primary key, $force_needed algorithm=copy
if (`select '$alter_algorithm' = 'instant' or  '$alter_algorithm' = 'nocopy'`)
{
 --let $alter_error=ER_CANT_DROP_FIELD_OR_KEY
}
--error 0,$alter_error
--eval alter table t1  drop column h, $force_needed algorithm=copy
if ($alter_algorithm == 'copy')
{
--eval alter table tmp_tbl add primary key(h), $force_needed algorithm=copy
--eval alter table tmp_tbl drop primary key, $force_needed algorithm=copy
--eval alter table tmp_tbl  drop column h, $force_needed algorithm=copy
}
--echo # show binlog and clear status
--sync_slave_with_master
if ($show_binlog)
{
  --source include/show_binlog_events.inc
}
reset master;
--connection master

## NOTE force_needed and algorithm will not be used for system versioning
#  | ADD PERIOD FOR SYSTEM_TIME (start_column_name, end_column_name)
#  | ADD SYSTEM VERSIONING
#  | DROP SYSTEM VERSIONING
if (`select '$alter_algorithm' = 'instant' or  '$alter_algorithm' = 'nocopy'`)
{
 --let $alter_error=ER_ALTER_OPERATION_NOT_SUPPORTED
}
--error 0,$alter_error
--eval alter table t1 add column f varchar(100) after b, add column g varchar(100) first ,add column  h char, $force_needed algorithm=$alter_algorithm
--eval alter table t1 add period for system_time(f,h)
--eval alter table t1 add system versioning
--eval alter table t1 drop system versioning
if (`select '$alter_algorithm' = 'instant' or  '$alter_algorithm' = 'nocopy'`)
{
 --let $alter_error=ER_CANT_DROP_FIELD_OR_KEY
}
--error 0,$alter_error
--eval alter table t1  drop column f, drop column g , drop column h, $force_needed algorithm=$alter_algorithm
--echo # show binlog and clear status
if ($alter_algorithm == 'copy')
{
--eval alter table tmp_tbl add column f varchar(100) after b, add column g varchar(100) first ,add column  h char, $force_needed algorithm=$alter_algorithm
--eval alter table tmp_tbl drop column f, drop column g , drop column h, $force_needed algorithm=$alter_algorithm
}
--sync_slave_with_master
if ($show_binlog)
{
  --source include/show_binlog_events.inc
}
reset master;
--connection master


#  | ALTER [COLUMN] col_name SET DEFAULT literal | (expression)
#  | ALTER [COLUMN] col_name DROP DEFAULT
#  | CHANGE [COLUMN] [IF EXISTS] old_col_name new_col_name column_definition
#        [FIRST|AFTER col_name]
#  | MODIFY [COLUMN] [IF EXISTS] col_name column_definition
#        [FIRST | AFTER col_name]
if (`select '$alter_algorithm' = 'instant' or  '$alter_algorithm' = 'nocopy'`)
{
 --let $alter_error=ER_ALTER_OPERATION_NOT_SUPPORTED
}
--error 0,$alter_error
--eval alter table t1 add column f varchar(100) after b,add column g varchar(100) first ,add column  h char, $force_needed algorithm=$alter_algorithm ;
if (`select '$alter_algorithm' = 'instant' or  '$alter_algorithm' = 'nocopy'`)
{
 --let $alter_error=ER_BAD_FIELD_ERROR
}
--error 0,$alter_error
--eval alter table t1 alter column f set default "****", $force_needed algorithm=$alter_algorithm ;
if (`select '$alter_algorithm' = 'instant' or  '$alter_algorithm' = 'nocopy'`)
{
 --let $alter_error=ER_BAD_FIELD_ERROR
}
--error 0,$alter_error
--eval alter table t1 alter column f drop default, $force_needed algorithm=$alter_algorithm ;
if (`select '$alter_algorithm' = 'instant' or  '$alter_algorithm' = 'nocopy'`)
{
 --let $alter_error=ER_BAD_FIELD_ERROR
}
--error 0,$alter_error
--eval alter table t1 change column g new_g char, $force_needed algorithm=copy;
if (`select '$alter_algorithm' = 'instant' or  '$alter_algorithm' = 'nocopy'`)
{
 --let $alter_error=ER_BAD_FIELD_ERROR
}
--error 0,$alter_error
--eval alter table t1 modify column h varchar(100), $force_needed algorithm=copy;
if (`select '$alter_algorithm' = 'instant' or  '$alter_algorithm' = 'nocopy'`)
{
 --let $alter_error=ER_CANT_DROP_FIELD_OR_KEY
}
--error 0,$alter_error
--eval alter table t1 drop column new_g ,drop column f, drop column h, $force_needed algorithm=$alter_algorithm ;
if ($alter_algorithm == 'copy')
{
--eval alter table tmp_tbl add column f varchar(100) after b,add column g varchar(100) first ,add column  h char, $force_needed algorithm=$alter_algorithm ;
--eval alter table tmp_tbl alter column f set default "****", $force_needed algorithm=$alter_algorithm ;
--eval alter table tmp_tbl alter column f drop default, $force_needed algorithm=$alter_algorithm ;
--eval alter table tmp_tbl change column g new_g char, $force_needed algorithm=copy;
--eval alter table tmp_tbl modify column h varchar(100), $force_needed algorithm=copy;
--eval alter table tmp_tbl drop column new_g ,drop column f, drop column h, $force_needed algorithm=$alter_algorithm ;
}
--echo # show binlog and clear status
--sync_slave_with_master
if ($show_binlog)
{
  --source include/show_binlog_events.inc
}
reset master;
--connection master

#   | DISABLE KEYS
#   | ENABLE KEYS
#   | RENAME [TO] new_tbl_name
#   | ORDER BY col_name [, col_name] ...
#   | RENAME COLUMN old_col_name TO new_col_name
#   | RENAME {INDEX|KEY} old_index_name TO new_index_name
#   | CONVERT TO CHARACTER SET charset_name [COLLATE collation_name]
#   | [DEFAULT] CHARACTER SET [=] charset_name
#   | [DEFAULT] COLLATE [=] collation_name
#   | DISCARD TABLESPACE
#   | IMPORT TABLESPACE
if (`select '$alter_algorithm' = 'instant' or  '$alter_algorithm' = 'nocopy'`)
{
 --let $alter_error=ER_ALTER_OPERATION_NOT_SUPPORTED
}
--error 0,$alter_error
--eval alter table t1 add column f varchar(100) after b,add column g varchar(100) first ,add column  h char, $force_needed algorithm=$alter_algorithm
if (`select '$alter_algorithm' = 'instant' or  '$alter_algorithm' = 'nocopy'`)
{
 --let $alter_error=ER_KEY_COLUMN_DOES_NOT_EXIST
}
--error 0,$alter_error
--eval alter table t1 add index if not exists index_1(f), $force_needed algorithm=$alter_algorithm
--eval alter table t1 disable keys, $force_needed algorithm=copy
--eval alter table t1 enable keys, $force_needed algorithm=copy
if (`select '$alter_algorithm' = 'instant' or  '$alter_algorithm' = 'nocopy'`)
{
 --let $alter_error=ER_ALTER_OPERATION_NOT_SUPPORTED
}
--error 0,$alter_error
--eval alter table t1 rename t2, $force_needed algorithm=$alter_algorithm
if (`select '$alter_algorithm' = 'instant' or  '$alter_algorithm' = 'nocopy'`)
{
 --let $alter_error=ER_NO_SUCH_TABLE
}
--error 0,$alter_error
--eval alter table t2 rename t1, $force_needed algorithm=$alter_algorithm
--eval alter table a1 order by a
if (`select '$alter_algorithm' = 'instant' or  '$alter_algorithm' = 'nocopy'`)
{
 --let $alter_error=ER_BAD_FIELD_ERROR
}
--error 0,$alter_error
--eval alter table t1 rename column f to new_f, $force_needed algorithm=copy
--eval alter table t1 convert to character set 'utf8', $force_needed algorithm=copy
--eval alter table t1 default character set 'utf8', $force_needed algorithm=copy
--eval alter table t1 default collate 'utf8_icelandic_ci', $force_needed algorithm=copy
if (`select '$alter_algorithm' = 'instant' or  '$alter_algorithm' = 'nocopy'`)
{
 --let $alter_error=ER_CANT_DROP_FIELD_OR_KEY
}
--error 0,$alter_error
--eval alter table t1 drop column new_f ,drop column g, drop column h, $force_needed algorithm=$alter_algorithm
if ($alter_algorithm == 'copy')
{
--eval alter table tmp_tbl add column f varchar(100) after b,add column g varchar(100) first ,add column  h char, $force_needed algorithm=$alter_algorithm
--eval alter table tmp_tbl add index if not exists index_1(f), $force_needed algorithm=$alter_algorithm
--eval alter table tmp_tbl disable keys, $force_needed algorithm=copy
--eval alter table tmp_tbl enable keys, $force_needed algorithm=copy
--eval alter table a1 order by a
--eval alter table tmp_tbl rename column f to new_f, $force_needed algorithm=copy
--eval alter table tmp_tbl convert to character set 'utf8', $force_needed algorithm=copy
--eval alter table tmp_tbl default character set 'utf8', $force_needed algorithm=copy
--eval alter table tmp_tbl default collate 'utf8_icelandic_ci', $force_needed algorithm=copy
--eval alter table tmp_tbl drop column new_f ,drop column g, drop column h, $force_needed algorithm=$alter_algorithm
}
##--eval alter table t1 discard tablespace;
######--eval alter table t1 import tablespace;

--echo # show binlog and clear status
--sync_slave_with_master
if ($show_binlog)
{
  --source include/show_binlog_events.inc
}
reset master;
--connection master

# Only add partition and remove partition is tested
#  | ADD PARTITION (partition_definition)
#  | REMOVE PARTITIONING
if (`select '$alter_algorithm' = 'instant' or  '$alter_algorithm' = 'nocopy'`)
{
 --let $alter_error=ER_ALTER_OPERATION_NOT_SUPPORTED
}
--error 0,$alter_error
--eval alter table t1 add column f varchar(100) after b,add column g varchar(100) first ,add column  h char, $force_needed algorithm=$alter_algorithm
--eval alter table t1  partition  by hash(b) partitions 8
--eval alter table t1  remove partitioning
if (`select '$alter_algorithm' = 'instant' or  '$alter_algorithm' = 'nocopy'`)
{
 --let $alter_error=ER_CANT_DROP_FIELD_OR_KEY
}
--error 0,$alter_error
--eval alter table t1 drop column f ,drop column g, drop column h, $force_needed algorithm=$alter_algorithm
if ($alter_algorithm == 'copy')
{
--eval alter table tmp_tbl add column f varchar(100) after b,add column g varchar(100) first ,add column  h char, $force_needed algorithm=$alter_algorithm
--eval alter table tmp_tbl drop column f ,drop column g, drop column h, $force_needed algorithm=$alter_algorithm
}
--echo # show binlog and clear status
--sync_slave_with_master
if ($show_binlog)
{
  --source include/show_binlog_events.inc
}
reset master;
--connection master

# clean master/slave
--connection master
drop table t1, a1;
drop temporary table tmp_tbl;
--sync_slave_with_master
--source include/stop_slave.inc
--eval set global slave_parallel_threads = $slave_parallel_threads;
--eval set global slave_parallel_mode = $slave_parallel_mode;
--eval set global gtid_strict_mode = $gtid_strict_mode;
--source include/start_slave.inc
