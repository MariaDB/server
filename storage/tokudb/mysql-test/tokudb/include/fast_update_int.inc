insert into t values (1,0), (2,0), (3,0);
select * from t;

# set is fast
update t set x = 100 where id = 2;
select * from t;

# increment is fast
update t set x = x + 1 where id = 3;
select * from t;

# decrement is fast
update t set x = x - 1 where id = 3;
select * from t;

# field=field+constant is fast
update t set x = x + 100 where id = 3;
select * from t;

# field=field-constant is fast
update t set x = x - 100 where id = 3;
select * from t;

# field=constant+field is not yet fast
--replace_regex /MariaDB/XYZ/ /MySQL/XYZ/
--error ER_UNSUPPORTED_EXTENSION
update t set x = 1 + x where id = 1;

# field=-field is not yet fast
--replace_regex /MariaDB/XYZ/ /MySQL/XYZ/
--error ER_UNSUPPORTED_EXTENSION
update t set x = -x where id = 1;

# yes, we can update a field in a non-existent row and the row is not inserted
update t set x = x + 1 where id = 100;
select * from t;

# range updates are not yet fast
--replace_regex /MariaDB/XYZ/ /MySQL/XYZ/
--error ER_UNSUPPORTED_EXTENSION
update t set x = x + 1 where 1 <= id and id < 100;

# full table updates are not yet fast
--replace_regex /MariaDB/XYZ/ /MySQL/XYZ/
--error ER_UNSUPPORTED_EXTENSION
update t set x = x + 1;

drop table t;
