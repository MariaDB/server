-- source include/have_innodb.inc

set @@session.time_zone='+00:00';
select ifnull(max(trx_id), 0) into @start_trx_id from information_schema.innodb_vtq;

delimiter ~~;
create procedure if not exists verify_vtq()
begin
  set @i= 0;
  select
    @i:= @i + 1 as No,
    trx_id > 0 as A,
    begin_ts > '1-1-1 0:0:0' as B,
    commit_ts > begin_ts as C,
    concurr_trx is null as D
  from information_schema.innodb_vtq
  where trx_id > @start_trx_id;
  select ifnull(max(trx_id), 0)
  into @start_trx_id
  from information_schema.innodb_vtq;
end~~
delimiter ;~~
