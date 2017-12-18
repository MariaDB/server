--disable_query_log
source include/have_innodb.inc;

set @@session.time_zone='+00:00';
select ifnull(max(transaction_id), 0) into @start_trx_id from mysql.transaction_registry;
set @test_start=now(6);

delimiter ~~;
create procedure if not exists verify_vtq()
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

create function if not exists default_engine()
returns varchar(255)
deterministic
begin
  declare e varchar(255);
  select engine from information_schema.engines where support='DEFAULT' into e;
  return e;
end~~

create function if not exists non_default_engine()
returns varchar(255)
deterministic
begin
  if default_engine() = 'InnoDB' then
    return 'MyISAM';
  end if;
  return 'InnoDB';
end~~

create function if not exists sys_datatype(engine varchar(255))
returns varchar(255)
deterministic
begin
  if engine = 'InnoDB' then
    return 'bigint(20) unsigned';
  elseif engine = 'MyISAM' then
    return 'timestamp(6)';
  end if;
  return NULL;
end~~

create function if not exists current_row(sys_trx_end varbinary(255))
returns int
deterministic
begin
  declare continue handler for sqlwarning begin end;
  return sys_trx_end = timestamp'2038-01-19 03:14:07.999999'
    or sys_trx_end = 18446744073709551615;
end~~

create function if not exists sys_commit_ts(sys_field varchar(255))
returns varchar(255)
deterministic
begin
  if default_engine() = 'InnoDB' then
    return concat('vtq_commit_ts(', sys_field, ')');
  elseif default_engine() = 'MyISAM' then
    return sys_field;
  end if;
  return NULL;
end~~

create procedure if not exists verify_vtq_dummy(recs int)
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

create procedure concat_exec2(a varchar(255), b varchar(255))
begin
  prepare stmt from concat(a, b);
  execute stmt;
  deallocate prepare stmt;
end~~

create procedure concat_exec3(a varchar(255), b varchar(255), c varchar(255))
begin
  prepare stmt from concat(a, b, c);
  execute stmt;
  deallocate prepare stmt;
end~~
delimiter ;~~

let $default_engine= `select default_engine()`;
let $non_default_engine= `select non_default_engine()`;
let $sys_datatype= timestamp(6);
let $sys_datatype_expl= timestamp(6);
let $sys_datatype_uc= TIMESTAMP(6);
let $sys_datatype_expl_uc= TIMESTAMP(6);

let $non_sys_datatype= `select sys_datatype(non_default_engine())`;
let $non_sys_datatype_uc= `select upper(sys_datatype(non_default_engine()))`;
let $sys_datatype_null= $sys_datatype NULL DEFAULT NULL;
let $sys_datatype_default_null= $sys_datatype DEFAULT NULL;
let $sys_datatype_not_null= $sys_datatype NOT NULL DEFAULT '0000-00-00 00:00:00.000000';
let $non_sys_datatype_null= $non_sys_datatype NULL;

if ($MTR_COMBINATION_MYISAM)
{
  --let $MTR_COMBINATION_TIMESTAMP= 1
}
if ($MTR_COMBINATION_TRX_ID)
{
  let $sys_datatype_expl= bigint(20) unsigned;
  let $sys_datatype_expl_uc= BIGINT(20) UNSIGNED;
}
--enable_query_log
