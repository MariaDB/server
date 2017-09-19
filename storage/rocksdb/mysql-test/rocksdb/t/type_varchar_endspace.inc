--echo #
--echo # Issue 257: Sort order for varchars is different between
--echo #            MyISAM/InnoDB vs MyRocks
--echo #

eval create table t1 (
  pk varchar(64) $character_set_collate,
  col1 varchar(64),
  primary key (pk)
);
insert into t1 values ('a','a');
--error ER_DUP_ENTRY
insert into t1 values ('a ', 'a-space');

insert into t1 values('b  ', 'b-2x-space');
--error ER_DUP_ENTRY
insert into t1 values ('b', 'b');

select pk, hex(pk), col1 from t1;

insert into t1 values ('a\t', 'a-tab');
insert into t1 values ('a \t', 'a-space-tab');
select pk, hex(pk), col1 from t1 order by pk;

--echo # Try longer values
--error ER_DUP_ENTRY
insert into t1 values (concat('a', repeat(' ',10)), 'a-10-x-space');

insert into t1 values (concat('c', repeat(' ',10)), 'c-10-x-space');
select * from t1;

drop table t1;

--echo # Secondary index
eval create table t1 (
  pk int not null primary key,
  col1 varchar(64) $character_set_collate,
  col2 varchar(64),
  key (col1)
);
insert into t1 values (0, 'ab', 'a-b');
insert into t1 values (1, 'a ', 'a-space');
insert into t1 values (2, 'a',  'a');
insert into t1 values (3, 'a  \t', 'a-tab');

analyze table t1;
--echo # Must show 'using index' for latin1_bin and utf8_bin:
--replace_column 9 #
explain
select col1, hex(col1) from t1;
select col1, hex(col1) from t1;

--echo # Must show 'using index' for latin1_bin and utf8_bin:
--replace_column 4 # 9 #
explain
select col1, hex(col1) from t1 force index(col1) where col1 < 'b';
select col1, hex(col1) from t1 force index(col1) where col1 < 'b';

delete from t1;
insert into t1 values(10, '', 'empty');
insert into t1 values(11, repeat(' ', 8), '8x-space');
insert into t1 values(12, repeat(' ', 16), '16x-space');
insert into t1 values(13, repeat(' ', 24), '24x-space');
insert into t1 values(14, concat(repeat(' ', 16),'a'), '16x-space-a');

insert into t1 values(21, repeat(' ', 9), '9x-space');
insert into t1 values(22, repeat(' ',17), '17x-space');
insert into t1 values(23, repeat(' ',18), '18x-space');

--replace_column 4 # 9 #
explain
select pk, col1, hex(col1), length(col1) from t1;
select pk, col1, hex(col1), length(col1) from t1;
drop table t1;

create table t1 (pk int primary key, a varchar(512), key(a)) engine=rocksdb;
insert into t1 values (1, concat('a', repeat(' ', 300)));
insert into t1 values (2, concat('b', repeat(' ', 300)));

select pk,length(a) from t1 force index(a) where a < 'zz';
select pk,length(a),rtrim(a) from t1 force index(a) where a < 'zz';
select pk,length(a),rtrim(a) from t1 ignore index(a) where a < 'zz';

drop table t1;

