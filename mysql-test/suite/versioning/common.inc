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
  select lower(engine) from information_schema.engines where support='DEFAULT' into e;
  return e;
end~~

create function if not exists sys_datatype()
returns varchar(255)
deterministic
begin
  if default_engine() = 'innodb' then
    return 'bigint unsigned';
  elseif default_engine() = 'myisam' then
    return 'timestamp(6)';
  end if;
  return NULL;
end~~

create function if not exists sys_commit_ts(sys_field varchar(255))
returns varchar(255)
deterministic
begin
  if default_engine() = 'innodb' then
    return concat('vtq_commit_ts(', sys_field, ')');
  elseif default_engine() = 'myisam' then
    return sys_field;
  end if;
  return NULL;
end~~

create procedure if not exists innodb_verify_vtq(recs int)
begin
  declare i int default 1;
  if default_engine() = 'innodb' then
    call verify_vtq;
  elseif default_engine() = 'myisam' then
    create temporary table tmp (No int, A bool, B bool, C bool, D bool);
    while i <= recs do
      insert into tmp values (i, 1, 1, 1, 1);
      set i= i + 1;
    end while;
    select * from tmp;
    drop table tmp;
  end if;
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
let sys_datatype= `select sys_datatype()`;
