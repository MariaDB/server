--source include/not_embedded.inc

if (`select count(*) = 0 from information_schema.plugins where plugin_name = 'unix_socket' and plugin_status='active'`)
{
  --skip Needs unix_socket plugin
}

if (!$USER) {
  skip USER variable is undefined;
}

if (`SELECT count(*) <> 0 FROM mysql.user WHERE user = '$USER'`) {
  skip \$USER=$USER which exists in mysql.user;
}
