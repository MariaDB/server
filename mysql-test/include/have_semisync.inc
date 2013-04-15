if (`select count(*) < 2 from information_schema.plugins where plugin_name like 'rpl_semi_sync_%'`)
{
  --skip Test requires semisync plugins
}
