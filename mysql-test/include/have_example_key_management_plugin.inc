-- source encryption_algorithms.inc

if (`select count(*) = 0 from information_schema.plugins
     where plugin_name = 'example_key_management' and plugin_status='active'`)
{
  --skip Needs example_key_management
}
