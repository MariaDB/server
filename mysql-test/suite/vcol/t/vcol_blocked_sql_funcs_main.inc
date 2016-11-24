################################################################################
# inc/vcol_blocked_sql_funcs_main.inc                                          #
#                                                                              #
# Purpose:                                                                     #
#  Tests around sql functions                                                  #
#                                                                              #
#                                                                              #
#------------------------------------------------------------------------------#
# Original Author: Andrey Zhakov                                               #
# Original Date: 2008-08-31                                                    #
# Change Author: Oleksandr Byelkin (Monty program Ab)
# Date: 2009-03-24 
# Change: Syntax changed
################################################################################

#
# NOTE:  All SQL functions should be rejected, otherwise BUG.
# As PERSISTANT has higher level checks than VIRTUAL, we use VIRTUAL
# to check for things that should not work for either VIRTUAL or PERSISTENT
#

--echo # RAND()
create or replace table t1 (b double as (rand()));
-- error ER_GENERATED_COLUMN_FUNCTION_IS_NOT_ALLOWED
create or replace table t1 (b double as (rand()) PERSISTENT);

--echo # LOAD_FILE()
-- error ER_GENERATED_COLUMN_FUNCTION_IS_NOT_ALLOWED
create or replace table t1 (a varchar(64), b varchar(1024) as (load_file(a)));

--echo # CURDATE()
-- error ER_GENERATED_COLUMN_FUNCTION_IS_NOT_ALLOWED
create or replace table t1 (a datetime as (curdate()) PERSISTENT);

--echo # CURRENT_DATE(), CURRENT_DATE
-- error ER_GENERATED_COLUMN_FUNCTION_IS_NOT_ALLOWED
create or replace table t1 (a datetime as (current_date) PERSISTENT);
-- error ER_GENERATED_COLUMN_FUNCTION_IS_NOT_ALLOWED
create or replace table t1 (a datetime as (current_date()) PERSISTENT);

--echo # CURRENT_TIME(), CURRENT_TIME
-- error ER_GENERATED_COLUMN_FUNCTION_IS_NOT_ALLOWED
create or replace table t1 (a datetime as (current_time) PERSISTENT);
-- error ER_GENERATED_COLUMN_FUNCTION_IS_NOT_ALLOWED
create or replace table t1 (a datetime as (current_time()) PERSISTENT);

--echo # CURRENT_TIMESTAMP(), CURRENT_TIMESTAMP
-- error ER_GENERATED_COLUMN_FUNCTION_IS_NOT_ALLOWED
create or replace table t1 (a datetime as (current_timestamp()) PERSISTENT);
-- error ER_GENERATED_COLUMN_FUNCTION_IS_NOT_ALLOWED
create or replace table t1 (a datetime as (current_timestamp) PERSISTENT);

--echo # CURTIME()
-- error ER_GENERATED_COLUMN_FUNCTION_IS_NOT_ALLOWED
create or replace table t1 (a datetime as (curtime()) PERSISTENT);

--echo # LOCALTIME(), LOCALTIME
-- error ER_GENERATED_COLUMN_FUNCTION_IS_NOT_ALLOWED
create or replace table t1 (a datetime, b varchar(10) as (localtime()) PERSISTENT); 
-- error ER_GENERATED_COLUMN_FUNCTION_IS_NOT_ALLOWED
create or replace table t1 (a datetime, b varchar(10) as (localtime) PERSISTENT); 

--echo # LOCALTIMESTAMP, LOCALTIMESTAMP()(v4.0.6)
-- error ER_GENERATED_COLUMN_FUNCTION_IS_NOT_ALLOWED
create or replace table t1 (a datetime, b varchar(10) as (localtimestamp()) PERSISTENT);
-- error ER_GENERATED_COLUMN_FUNCTION_IS_NOT_ALLOWED
create or replace table t1 (a datetime, b varchar(10) as (localtimestamp) PERSISTENT);

--echo # NOW()
-- error ER_GENERATED_COLUMN_FUNCTION_IS_NOT_ALLOWED
create or replace table t1 (a datetime, b varchar(10) as (now()) PERSISTENT);

--echo # SYSDATE()
-- error ER_GENERATED_COLUMN_FUNCTION_IS_NOT_ALLOWED
create or replace table t1 (a int, b varchar(10) as (sysdate()) PERSISTENT);

--echo # UNIX_TIMESTAMP()
-- error ER_GENERATED_COLUMN_FUNCTION_IS_NOT_ALLOWED
create or replace table t1 (a datetime, b datetime as (unix_timestamp()) PERSISTENT);

--echo # UTC_DATE()
-- error ER_GENERATED_COLUMN_FUNCTION_IS_NOT_ALLOWED
create or replace table t1 (a datetime, b datetime as (utc_date()) PERSISTENT);

--echo # UTC_TIME()
-- error ER_GENERATED_COLUMN_FUNCTION_IS_NOT_ALLOWED
create or replace table t1 (a datetime, b datetime as (utc_time()) PERSISTENT);

--echo # UTC_TIMESTAMP()
-- error ER_GENERATED_COLUMN_FUNCTION_IS_NOT_ALLOWED
create or replace table t1 (a datetime, b datetime as (utc_timestamp()) PERSISTENT);

--echo # WEEK() - one argument version
-- error ER_GENERATED_COLUMN_FUNCTION_IS_NOT_ALLOWED
create or replace table t1 (a datetime, b datetime as (week(a)) PERSISTENT);

--echo # MATCH()
--error ER_GENERATED_COLUMN_FUNCTION_IS_NOT_ALLOWED
  create or replace table t1 (a varchar(32), b bool as (match a against ('sample text')) PERSISTENT);

--echo # BENCHMARK()
-- error ER_GENERATED_COLUMN_FUNCTION_IS_NOT_ALLOWED
create or replace table t1 (a varchar(1024), b varchar(1024) as (benchmark(a,3)));

--echo # CHARSET()
create or replace table t1 (a varchar(64), b varchar(64) as (charset(a)) PERSISTENT);

--echo # COERCIBILITY()
create or replace table t1 (a varchar(64), b int as (coercibility(a)) PERSISTENT);

--echo # COLLATION()
create or replace table t1 (a varchar(64), b varchar(64) as (collation(a)) PERSISTENT);

--echo # CONNECTION_ID()
create or replace table t1 (a int as (connection_id()));
-- error ER_GENERATED_COLUMN_FUNCTION_IS_NOT_ALLOWED
create or replace table t1 (a int as (connection_id()) PERSISTENT);

--echo # DATABASE()
create or replace table t1 (a varchar(32) as (database()));
-- error ER_GENERATED_COLUMN_FUNCTION_IS_NOT_ALLOWED
create or replace table t1 (a varchar(1024), b varchar(1024) as (database()) PERSISTENT);

--echo # FOUND_ROWS()
-- error ER_GENERATED_COLUMN_FUNCTION_IS_NOT_ALLOWED
create or replace table t1 (a varchar(1024), b varchar(1024) as (found_rows()));

--echo # GET_LOCK()
-- error ER_GENERATED_COLUMN_FUNCTION_IS_NOT_ALLOWED
create or replace table t1 (a varchar(1024), b varchar(1024) as (get_lock(a,10)));

--echo # IS_FREE_LOCK()
-- error ER_GENERATED_COLUMN_FUNCTION_IS_NOT_ALLOWED
create or replace table t1 (a varchar(1024), b varchar(1024) as (is_free_lock(a)));

--echo # IS_USED_LOCK()
-- error ER_GENERATED_COLUMN_FUNCTION_IS_NOT_ALLOWED
create or replace table t1 (a varchar(1024), b varchar(1024) as (is_used_lock(a)));

--echo # LAST_INSERT_ID()
-- error ER_GENERATED_COLUMN_FUNCTION_IS_NOT_ALLOWED
create or replace table t1 (a int as (last_insert_id()));

--echo # MASTER_POS_WAIT()
-- error ER_GENERATED_COLUMN_FUNCTION_IS_NOT_ALLOWED
create or replace table t1 (a varchar(32), b int as (master_pos_wait(a,0,2)));

--echo # NAME_CONST()
-- error ER_GENERATED_COLUMN_FUNCTION_IS_NOT_ALLOWED
create or replace table t1 (a varchar(32) as (name_const('test',1)));

--echo # RELEASE_LOCK()
-- error ER_GENERATED_COLUMN_FUNCTION_IS_NOT_ALLOWED
create or replace table t1 (a varchar(32), b int as (release_lock(a)));

--echo # ROW_COUNT()
-- error ER_GENERATED_COLUMN_FUNCTION_IS_NOT_ALLOWED
create or replace table t1 (a int as (row_count()));

--echo # SCHEMA()
-- error ER_GENERATED_COLUMN_FUNCTION_IS_NOT_ALLOWED
create or replace table t1 (a varchar(32) as (schema()) PERSISTENT);

--echo # SESSION_USER()
-- error ER_GENERATED_COLUMN_FUNCTION_IS_NOT_ALLOWED
create or replace table t1 (a varchar(32) as (session_user()) PERSISTENT);

--echo # SLEEP()
-- error ER_GENERATED_COLUMN_FUNCTION_IS_NOT_ALLOWED
create or replace table t1 (a int, b int as (sleep(a)));

--echo # SYSTEM_USER()
-- error ER_GENERATED_COLUMN_FUNCTION_IS_NOT_ALLOWED
create or replace table t1 (a varchar(32) as (system_user()) PERSISTENT);

--echo # USER()
-- error ER_GENERATED_COLUMN_FUNCTION_IS_NOT_ALLOWED
create or replace table t1 (a varchar(1024), b varchar(1024) as (user()) PERSISTENT);

--echo # UUID_SHORT()
-- error ER_GENERATED_COLUMN_FUNCTION_IS_NOT_ALLOWED
create or replace table t1 (a varchar(1024) as (uuid_short()) PERSISTENT);

--echo # UUID()
-- error ER_GENERATED_COLUMN_FUNCTION_IS_NOT_ALLOWED
create or replace table t1 (a varchar(1024) as (uuid()) PERSISTENT);

--echo # VALUES()
-- error ER_GENERATED_COLUMN_FUNCTION_IS_NOT_ALLOWED
create or replace table t1 (a varchar(1024), b varchar(1024) as (values(a)));

--echo # VERSION()
-- error ER_GENERATED_COLUMN_FUNCTION_IS_NOT_ALLOWED
create or replace table t1 (a varchar(1024), b varchar(1024) as (version()) PERSISTENT); 

--echo # ENCRYPT()
create or replace table t1 (a varchar(1024), b varchar(1024) as (encrypt(a)) PERSISTENT);

--echo # Stored procedures

delimiter //;
create procedure p1()
begin
  select current_user();
end //

create function f1()
returns int
begin
 return 1;
end //

delimiter ;//

-- error ER_GENERATED_COLUMN_FUNCTION_IS_NOT_ALLOWED
create or replace table t1 (a int as (p1()) PERSISTENT);
-- error ER_GENERATED_COLUMN_FUNCTION_IS_NOT_ALLOWED
create or replace table t1 (a int as (f1()) PERSISTENT);

drop procedure p1;
drop function f1;

--echo # Unknown functions
-- error ER_GENERATED_COLUMN_FUNCTION_IS_NOT_ALLOWED
create or replace table t1 (a int as (f1()) PERSISTENT);

--echo #
--echo # GROUP BY FUNCTIONS
--echo #

--echo # AVG()
-- error ER_GENERATED_COLUMN_FUNCTION_IS_NOT_ALLOWED
create or replace table t1 (a int, b int as (avg(a)));

--echo # BIT_AND()
-- error ER_GENERATED_COLUMN_FUNCTION_IS_NOT_ALLOWED
create or replace table t1 (a int, b int as (bit_and(a)));

--echo # BIT_OR()
-- error ER_GENERATED_COLUMN_FUNCTION_IS_NOT_ALLOWED
create or replace table t1 (a int, b int as (bit_or(a)));

--echo # BIT_XOR()
-- error ER_GENERATED_COLUMN_FUNCTION_IS_NOT_ALLOWED
create or replace table t1 (a int, b int as (bit_xor(a)));

--echo # COUNT(DISTINCT)
-- error ER_GENERATED_COLUMN_FUNCTION_IS_NOT_ALLOWED
create or replace table t1 (a int, b int as (count(distinct a)));

--echo # COUNT()
-- error ER_GENERATED_COLUMN_FUNCTION_IS_NOT_ALLOWED
create or replace table t1 (a int, b int as (count(a)));

--echo # GROUP_CONCAT()
-- error ER_GENERATED_COLUMN_FUNCTION_IS_NOT_ALLOWED
create or replace table t1 (a varchar(32), b int as (group_concat(a,'')));

--echo # MAX()
-- error ER_GENERATED_COLUMN_FUNCTION_IS_NOT_ALLOWED
create or replace table t1 (a int, b int as (max(a)));

--echo # MIN()
-- error ER_GENERATED_COLUMN_FUNCTION_IS_NOT_ALLOWED
create or replace table t1 (a int, b int as (min(a)));

--echo # STD()
-- error ER_GENERATED_COLUMN_FUNCTION_IS_NOT_ALLOWED
create or replace table t1 (a int, b int as (std(a)));

--echo # STDDEV_POP()
-- error ER_GENERATED_COLUMN_FUNCTION_IS_NOT_ALLOWED
create or replace table t1 (a int, b int as (stddev_pop(a)));

--echo # STDDEV_SAMP()
-- error ER_GENERATED_COLUMN_FUNCTION_IS_NOT_ALLOWED
create or replace table t1 (a int, b int as (stddev_samp(a)));

--echo # STDDEV()
-- error ER_GENERATED_COLUMN_FUNCTION_IS_NOT_ALLOWED
create or replace table t1 (a int, b int as (stddev(a)));

--echo # SUM()
-- error ER_GENERATED_COLUMN_FUNCTION_IS_NOT_ALLOWED
create or replace table t1 (a int, b int as (sum(a)));

--echo # VAR_POP()
-- error ER_GENERATED_COLUMN_FUNCTION_IS_NOT_ALLOWED
create or replace table t1 (a int, b int as (var_pop(a)));

--echo # VAR_SAMP()
-- error ER_GENERATED_COLUMN_FUNCTION_IS_NOT_ALLOWED
create or replace table t1 (a int, b int as (var_samp(a)));

--echo # VARIANCE()
-- error ER_GENERATED_COLUMN_FUNCTION_IS_NOT_ALLOWED
create or replace table t1 (a int, b int as (variance(a)));

--echo #
--echo # XML FUNCTIONS
--echo #

--echo # ExtractValue()
create or replace table t1 (a varchar(1024), b varchar(1024) as (ExtractValue(a,'//b[$@j]')) PERSISTENT);

--echo # UpdateXML()
create or replace table t1 (a varchar(1024), b varchar(1024) as (UpdateXML(a,'/a','<e>fff</e>')) PERSISTENT);

--echo #
--echo # Sub-selects
--echo #

create or replace table t1 (a int);
-- error ER_GENERATED_COLUMN_FUNCTION_IS_NOT_ALLOWED
create or replace table t2 (a int, b int as (select count(*) from t1));
drop table t1;

-- error ER_GENERATED_COLUMN_FUNCTION_IS_NOT_ALLOWED
create or replace table t1 (a int, b int as ((select 1)));
-- error ER_GENERATED_COLUMN_FUNCTION_IS_NOT_ALLOWED
create or replace table t1 (a int, b int as (a+(select 1)));

--echo #
--echo # SP functions
--echo #

--disable_warnings
drop function if exists sub1;
--enable_warnings
create function sub1(i int) returns int deterministic
  return i+1;
select sub1(1);
-- error ER_GENERATED_COLUMN_FUNCTION_IS_NOT_ALLOWED
create or replace table t1 (a int, b int as (a+sub3(1)));
drop function sub1;

--echo #
--echo # Long expression

let $tmp_long_string = `SELECT repeat('a',240)`;
eval create or replace table t1 (a int, b varchar(300) as (concat(a,'$tmp_long_string')));
drop table t1;
let $tmp_long_string = `SELECT repeat('a',243)`;
eval create or replace table t1 (a int, b varchar(16384) as (concat(a,'$tmp_long_string')));

--disable_query_log
let $tmp_long_string = `SELECT repeat('a',65535)`;
--error ER_EXPRESSION_IS_TOO_BIG
eval create or replace table t1 (a int, b varchar(16384) as (concat(a,'$tmp_long_string')));
--enable_query_log

--echo #
--echo # Constant expression
create or replace table t1 (a int as (PI()) PERSISTENT);

drop table if exists t1;
