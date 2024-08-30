# Check sargable_casefold rewrite for $collation

eval create table t1 (
  col1 varchar(32), 
  col2 varchar(32), 
  col3 char(32), 
  col4  text, 
  key(col1),
  key(col2),
  key(col3),
  key(col4(32))
) collate $collation;

insert into t1
select 
  concat('A-', seq),
  concat('A-', seq),
  concat('A-', seq),
  concat('A-', seq)
from seq_1_to_100;

analyze table t1 persistent for all;

--echo # Basic examples. All should use ref(col1):
explain
select * from t1 where upper(col1)='A-3';
select * from t1 where upper(col1)='A-3';

explain
select * from t1 where ucase(col1)='a-3';
select * from t1 where ucase(col1)='a-3';

explain select * from t1 where 'abc'=upper(col1);
explain select * from t1 where 'xyz'=ucase(col1);

create view v1 as select * from t1;
explain select * from v1 where 'abc'=upper(col1);
drop view v1;

explain select * from t1 where upper(col3)='a-3';
explain select * from t1 where upper(col4)='a-3';

# DROP TABLE t1 is missing intentionally here.

