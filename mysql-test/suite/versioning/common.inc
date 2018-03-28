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

create function if not exists sys_commit_ts(sys_field varchar(255))
returns varchar(255)
deterministic
begin
  if @@default_storage_engine = 'InnoDB' then
    return concat('vtq_commit_ts(', sys_field, ')');
  elseif @@default_storage_engine = 'MyISAM' then
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

let $default_engine= `select @@default_storage_engine`;
let $non_default_engine= `select if(@@default_storage_engine = 'InnoDB', 'MyISAM', 'InnoDB')`;
let $sys_datatype_expl= timestamp(6);
let $sys_datatype_expl_uc= TIMESTAMP(6);
let $sys_datatype_max= TIMESTAMP'2038-01-19 03:14:07.999999';

if ($MTR_COMBINATION_MYISAM)
{
  --let $MTR_COMBINATION_TIMESTAMP= 1
}
if ($MTR_COMBINATION_TRX_ID)
{
  let $sys_datatype_expl= bigint(20) unsigned;
  let $sys_datatype_expl_uc= BIGINT(20) UNSIGNED;
  let $sys_datatype_max= 18446744073709551615;
}
--enable_query_log
