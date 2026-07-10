if (!$TEST_VERSIONING_SO)
{
  --skip needs test_versioning plugin
}
source include/have_innodb.inc;
source sys_time.inc;
--disable_query_log

set @@session.time_zone='+00:00';
set @@global.time_zone='+00:00';
--disable_cursor_protocol
select ifnull(max(transaction_id), 0) into @start_trx_id from mysql.transaction_registry;
--enable_cursor_protocol
set @test_start=now(6);

delimiter ~~;
create procedure if not exists verify_trt()
begin
  set @i= 0;
  select
    @i:= @i + 1 as No,
    transaction_id > 0 as A,
    commit_id > transaction_id as B,
    begin_timestamp > @test_start as C,
    commit_timestamp >= begin_timestamp as D
  from mysql.transaction_registry
  where transaction_id > @start_trx_id;
  select ifnull(max(transaction_id), 0)
  into @start_trx_id
  from mysql.transaction_registry;
end~~

create procedure if not exists verify_trt_dummy(recs int)
begin
  declare i int default 1;
  create temporary table tmp (No int, A bool, B bool, C bool, D bool);
  while i <= recs do
    insert into tmp values (i, 1, 1, 1, 1);
    set i= i + 1;
  end while;
  select * from tmp;
  drop table tmp;
end~~

delimiter ;~~

let $default_engine= `select @@default_storage_engine`;
let $non_default_engine= `select if(@@default_storage_engine = 'InnoDB', 'MyISAM', 'InnoDB')`;
let $sys_datatype_expl= timestamp(6);
let $sys_datatype_expl_uc= TIMESTAMP(6);

if ($MTR_COMBINATION_MYISAM)
{
  --let $MTR_COMBINATION_TIMESTAMP= 1
}
if ($MTR_COMBINATION_TRADITIONAL)
{
  --let $MTR_COMBINATION_TIMESTAMP= 1
}
if ($MTR_COMBINATION_HEAP)
{
  --let $MTR_COMBINATION_TIMESTAMP= 1
}
if ($MTR_COMBINATION_TRX_ID)
{
  let $sys_datatype_expl= bigint(20) unsigned;
  let $sys_datatype_expl_uc= BIGINT(20) UNSIGNED;
  let $sys_datatype_max= 18446744073709551615;
  let $current_row= current_row;
}
if ($MTR_COMBINATION_TIMESTAMP)
{
  let $current_row= current_row_ts;
}

eval create or replace function current_row(sys_trx_end $sys_datatype_expl)
returns int
deterministic
  return sys_trx_end = $sys_datatype_max;

eval create or replace function current_row_ts(sys_trx_end timestamp(6))
returns int
deterministic
  return convert_tz(sys_trx_end, @@time_zone, '+00:00') = $sys_time_max;

delimiter ~~;
eval create or replace function check_row(row_start $sys_datatype_expl, row_end $sys_datatype_expl)
returns varchar(255)
deterministic
begin
  if row_end < row_start then
    return "ERROR: row_end < row_start";
  elseif row_end = row_start then
    return "ERROR: row_end == row_start";
  elseif $current_row(row_end) then
    return "CURRENT ROW";
  end if;
  return "HISTORICAL ROW";
end~~
delimiter ;~~

delimiter ~~;
eval create or replace function check_row_slave(row_start $sys_datatype_expl, row_end $sys_datatype_expl)
returns varchar(255)
deterministic
begin
  if $current_row(row_end) then
    return "CURRENT ROW";
  end if;
  return "HISTORICAL ROW";
end~~
delimiter ;~~

delimiter ~~;
eval create or replace function check_row_ts(row_start timestamp(6), row_end timestamp(6))
returns varchar(255)
deterministic
begin
  if row_end < row_start then
    return "ERROR: row_end < row_start";
  elseif row_end = row_start then
    return "ERROR: row_end == row_start";
  elseif current_row_ts(row_end) then
    return "CURRENT ROW";
  end if;
  return "HISTORICAL ROW";
end~~
delimiter ;~~

--enable_query_log
