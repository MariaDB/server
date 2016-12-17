################################################################################
# inc/gcol_blocked_sql_funcs_main.inc                                          #
#                                                                              #
# Purpose:                                                                     #
#  Tests around sql functions                                                  #
#                                                                              #
#                                                                              #
#------------------------------------------------------------------------------#
# Original Author: Andrey Zhakov                                               #
# Original Date: 2008-08-31                                                    #
# Change Author:                                                               #
# Change Date:                                                                 #
# Change:                                                                      #
################################################################################

#
# MySQL doesn't support them, but MariaDB does:
#
create or replace table t1 (b double generated always as (rand()) virtual);
create or replace table t1 (a datetime generated always as (curdate()) virtual);
create or replace table t1 (a datetime generated always as (current_date) virtual);
create or replace table t1 (a datetime generated always as (current_date()) virtual);
create or replace table t1 (a datetime generated always as (current_time) virtual);
create or replace table t1 (a datetime generated always as (current_time()) virtual);
create or replace table t1 (a datetime generated always as (current_timestamp()) virtual);
create or replace table t1 (a datetime generated always as (current_timestamp) virtual);
create or replace table t1 (a datetime generated always as (curtime()) virtual);
create or replace table t1 (a datetime, b varchar(10) generated always as (localtime()) virtual);
create or replace table t1 (a datetime, b varchar(10) generated always as (localtime) virtual);
create or replace table t1 (a datetime, b varchar(10) generated always as (localtimestamp()) virtual);
create or replace table t1 (a datetime, b varchar(10) generated always as (localtimestamp) virtual);
create or replace table t1 (a datetime, b varchar(10) generated always as (now()) virtual);
create or replace table t1 (a int, b varchar(10) generated always as (sysdate()) virtual);
create or replace table t1 (a datetime, b datetime generated always as (unix_timestamp()) virtual);
create or replace table t1 (a datetime, b datetime generated always as (utc_date()) virtual);
create or replace table t1 (a datetime, b datetime generated always as (utc_time()) virtual);
create or replace table t1 (a datetime, b datetime generated always as (utc_timestamp()) virtual);
create or replace table t1 (a int generated always as (connection_id()) virtual);
create or replace table t1 (a varchar(32) generated always as (current_user()) virtual);
create or replace table t1 (a varchar(32) generated always as (current_user) virtual);
create or replace table t1 (a varchar(1024), b varchar(1024) generated always as (database()) virtual);
create or replace table t1 (a varchar(32) generated always as (schema()) virtual);
create or replace table t1 (a varchar(32) generated always as (session_user()) virtual);
create or replace table t1 (a varchar(32) generated always as (system_user()) virtual);
create or replace table t1 (a varchar(1024), b varchar(1024) generated always as (user()) virtual);
create or replace table t1 (a varchar(1024) generated always as (uuid_short()) virtual);
create or replace table t1 (a varchar(1024) generated always as (uuid()) virtual);
create or replace table t1 (a varchar(1024), b varchar(1024) generated always as (version()) virtual);
create or replace table t1 (a varchar(1024), b varchar(1024) generated always as (encrypt(a)) virtual);
create or replace table t1 (a varchar(1024), b varchar(1024) generated always as (UpdateXML(a,'/a','<e>fff</e>')) virtual);
drop table t1;

#
# NOTE:  All SQL functions below should be rejected, otherwise BUG.
#

--echo # LOAD_FILE()
-- error ER_GENERATED_COLUMN_FUNCTION_IS_NOT_ALLOWED
create table t1 (a varchar(64), b varchar(1024) generated always as (load_file(a)) virtual);

--echo # MATCH()
if (!$skip_full_text_check)
{
  -- error ER_GENERATED_COLUMN_FUNCTION_IS_NOT_ALLOWED
  create table t1 (a varchar(32), b bool generated always as (match a against ('sample text')) virtual);
}

--echo # BENCHMARK()
-- error ER_GENERATED_COLUMN_FUNCTION_IS_NOT_ALLOWED
create table t1 (a varchar(1024), b varchar(1024) generated always as (benchmark(a,3)) virtual);

--echo # FOUND_ROWS()
-- error ER_GENERATED_COLUMN_FUNCTION_IS_NOT_ALLOWED
create table t1 (a varchar(1024), b varchar(1024) generated always as (found_rows()) virtual);

--echo # GET_LOCK()
-- error ER_GENERATED_COLUMN_FUNCTION_IS_NOT_ALLOWED
create table t1 (a varchar(1024), b varchar(1024) generated always as (get_lock(a,10)) virtual);

--echo # IS_FREE_LOCK()
-- error ER_GENERATED_COLUMN_FUNCTION_IS_NOT_ALLOWED
create table t1 (a varchar(1024), b varchar(1024) generated always as (is_free_lock(a)) virtual);

--echo # IS_USED_LOCK()
-- error ER_GENERATED_COLUMN_FUNCTION_IS_NOT_ALLOWED
create table t1 (a varchar(1024), b varchar(1024) generated always as (is_used_lock(a)) virtual);

--echo # LAST_INSERT_ID()
-- error ER_GENERATED_COLUMN_FUNCTION_IS_NOT_ALLOWED
create table t1 (a int generated always as (last_insert_id()) virtual);

--echo # MASTER_POS_WAIT()
-- error ER_GENERATED_COLUMN_FUNCTION_IS_NOT_ALLOWED
create table t1 (a varchar(32), b int generated always as (master_pos_wait(a,0,2)) virtual);

--echo # NAME_CONST()
-- error ER_GENERATED_COLUMN_FUNCTION_IS_NOT_ALLOWED
create table t1 (a varchar(32) generated always as (name_const('test',1)) virtual);

--echo # RELEASE_LOCK()
-- error ER_GENERATED_COLUMN_FUNCTION_IS_NOT_ALLOWED
create table t1 (a varchar(32), b int generated always as (release_lock(a)) virtual);

--echo # ROW_COUNT()
-- error ER_GENERATED_COLUMN_FUNCTION_IS_NOT_ALLOWED
create table t1 (a int generated always as (row_count()) virtual);

--echo # SLEEP()
-- error ER_GENERATED_COLUMN_FUNCTION_IS_NOT_ALLOWED
create table t1 (a int, b int generated always as (sleep(a)) virtual);

--echo # VALUES()
-- error ER_GENERATED_COLUMN_FUNCTION_IS_NOT_ALLOWED
create table t1 (a varchar(1024), b varchar(1024) generated always as (values(a)) virtual);

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
create table t1 (a int generated always as (p1()) virtual);
-- error ER_GENERATED_COLUMN_FUNCTION_IS_NOT_ALLOWED
create table t1 (a int generated always as (f1()) virtual);

drop procedure p1;
drop function f1;

--echo # Unknown functions
-- error ER_GENERATED_COLUMN_FUNCTION_IS_NOT_ALLOWED
create table t1 (a int generated always as (f1()) virtual);

--echo #
--echo # GROUP BY FUNCTIONS
--echo #

--echo # AVG()
-- error ER_GENERATED_COLUMN_FUNCTION_IS_NOT_ALLOWED
create table t1 (a int, b int generated always as (avg(a)) virtual);

--echo # BIT_AND()
-- error ER_GENERATED_COLUMN_FUNCTION_IS_NOT_ALLOWED
create table t1 (a int, b int generated always as (bit_and(a)) virtual);

--echo # BIT_OR()
-- error ER_GENERATED_COLUMN_FUNCTION_IS_NOT_ALLOWED
create table t1 (a int, b int generated always as (bit_or(a)) virtual);

--echo # BIT_XOR()
-- error ER_GENERATED_COLUMN_FUNCTION_IS_NOT_ALLOWED
create table t1 (a int, b int generated always as (bit_xor(a)) virtual);

--echo # COUNT(DISTINCT)
-- error ER_GENERATED_COLUMN_FUNCTION_IS_NOT_ALLOWED
create table t1 (a int, b int generated always as (count(distinct a)) virtual);

--echo # COUNT()
-- error ER_GENERATED_COLUMN_FUNCTION_IS_NOT_ALLOWED
create table t1 (a int, b int generated always as (count(a)) virtual);

--echo # GROUP_CONCAT()
-- error ER_GENERATED_COLUMN_FUNCTION_IS_NOT_ALLOWED
create table t1 (a varchar(32), b int generated always as (group_concat(a,'')) virtual);

--echo # MAX()
-- error ER_GENERATED_COLUMN_FUNCTION_IS_NOT_ALLOWED
create table t1 (a int, b int generated always as (max(a)) virtual);

--echo # MIN()
-- error ER_GENERATED_COLUMN_FUNCTION_IS_NOT_ALLOWED
create table t1 (a int, b int generated always as (min(a)) virtual);

--echo # STD()
-- error ER_GENERATED_COLUMN_FUNCTION_IS_NOT_ALLOWED
create table t1 (a int, b int generated always as (std(a)) virtual);

--echo # STDDEV_POP()
-- error ER_GENERATED_COLUMN_FUNCTION_IS_NOT_ALLOWED
create table t1 (a int, b int generated always as (stddev_pop(a)) virtual);

--echo # STDDEV_SAMP()
-- error ER_GENERATED_COLUMN_FUNCTION_IS_NOT_ALLOWED
create table t1 (a int, b int generated always as (stddev_samp(a)) virtual);

--echo # STDDEV()
-- error ER_GENERATED_COLUMN_FUNCTION_IS_NOT_ALLOWED
create table t1 (a int, b int generated always as (stddev(a)) virtual);

--echo # SUM()
-- error ER_GENERATED_COLUMN_FUNCTION_IS_NOT_ALLOWED
create table t1 (a int, b int generated always as (sum(a)) virtual);

--echo # VAR_POP()
-- error ER_GENERATED_COLUMN_FUNCTION_IS_NOT_ALLOWED
create table t1 (a int, b int generated always as (var_pop(a)) virtual);

--echo # VAR_SAMP()
-- error ER_GENERATED_COLUMN_FUNCTION_IS_NOT_ALLOWED
create table t1 (a int, b int generated always as (var_samp(a)) virtual);

--echo # VARIANCE()
-- error ER_GENERATED_COLUMN_FUNCTION_IS_NOT_ALLOWED
create table t1 (a int, b int generated always as (variance(a)) virtual);

--echo #
--echo # Sub-selects
--echo #

create table t1 (a int);
-- error ER_GENERATED_COLUMN_FUNCTION_IS_NOT_ALLOWED
create table t2 (a int, b int generated always as (select count(*) virtual from t1));
drop table t1;

--echo #
--echo # Long expression

let $tmp_long_string = `SELECT repeat('a',240)`;
eval create table t1 (a int, b varchar(300) generated always as (concat(a,'$tmp_long_string')) virtual);
drop table t1;
let $tmp_long_string = `SELECT repeat('a',243)`;
# Limit is lifted to 64K. TODO write a test for it.
# --error 1470
eval create table t1 (a int, b varchar(300) generated always as (concat(a,'$tmp_long_string')) virtual);
drop table t1;

--echo #
--echo # Constant expression
create table t1 (a int generated always as (PI()) virtual);
drop table t1;

--echo # bug#21098119: GCOL WITH MATCH/AGAINST -->
--echo # ASSERTION FAILED: TR && TR->TABLE->FILE
--echo #
create table t1 (a int);
--error ER_GENERATED_COLUMN_FUNCTION_IS_NOT_ALLOWED
alter table t1 add column r blob generated always
as (match(a) against ('' in boolean mode)) virtual;
drop table t1;
