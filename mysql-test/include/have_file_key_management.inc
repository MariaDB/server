if (`SELECT COUNT(*)=0 FROM INFORMATION_SCHEMA.PLUGINS WHERE PLUGIN_NAME = 'file_key_management' AND PLUGIN_STATUS='ACTIVE'`)
{
  --skip Test requires file_key_management plugin
}

#
# This file is only included when using encryption. As all encryption test are
# very slow with valgrind, lets disable these if not run with --big
#
--source include/no_valgrind_without_big.inc
