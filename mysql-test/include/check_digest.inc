#
# check_digest() checks digests validity from performance_schema.events_statements_summary_by_digest
#

--disable_query_log
create table test._digests(d varchar(32) primary key);

delimiter ~~;
create function test.check_digest(digest varchar(255))
returns tinyint
not deterministic
begin
  declare digest_exists tinyint;
  if length(digest) != 32 or conv(digest, 16, 10) = 0 then
    return 0;
  end if;
  select exists (select d from test._digests where d = digest) into digest_exists;
  if digest_exists then
    return 0;
  end if;
  #insert into test._digests values (digest);
  return 1;
end~~

create procedure test.reset_digests()
begin
  delete from test._digests;
end~~
delimiter ;~~

--disable_result_log
call sys.ps_truncate_all_tables(false);
--enable_result_log
--enable_query_log
