source include/not_embedded.inc;

call mtr.add_suppression('failed to read private_key.pem: 2 "No such file or directory"');

if (`select count(*) = 0 from information_schema.plugins where plugin_name = 'caching_sha2_password'`)
{
  --skip Needs caching_sha2_password plugin
}

show status like 'caching_sha2_password%';

create user test1@'%' identified via caching_sha2_password using PASSWORD('pwd');
create user test2@'%' identified via caching_sha2_password;
show grants for test2@'%';

create procedure checkme() sql security invoker
  select user(), current_user(), variable_value > '' as 'have_ssl'
    from information_schema.session_status
    where variable_name='ssl_cipher';

grant execute on test.* to test1@'%', test2@'%';
