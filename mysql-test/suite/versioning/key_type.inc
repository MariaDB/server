--disable_query_log
if ($MTR_COMBINATION_UNIQUE)
{
    set @KEY_TYPE= 'unique';
}
if ($MTR_COMBINATION_PK)
{
    set @KEY_TYPE= 'primary key';
}

delimiter ~~;
create procedure create_table(name varchar(255), cols varchar(255))
begin
  if (cols is null or cols = '') then
    set cols= '';
  else
    set cols= concat(', ', cols);
  end if;
  set @str= concat('create or replace table ', name, '(id int ', @KEY_TYPE, cols, ') with system versioning');
  prepare stmt from @str; execute stmt; drop prepare stmt;
end~~
delimiter ;~~
--enable_query_log
