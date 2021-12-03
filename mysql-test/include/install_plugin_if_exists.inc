--echo #
--echo # MDEV-16294: INSTALL PLUGIN IF NOT EXISTS / UNINSTALL PLUGIN IF EXISTS
--echo #
--echo # INSTALL IF NOT EXISTS PLUGIN name SONAME library /
--echo # UNINSTALL IF EXISTS PLUGIN|SONAME name
--echo #

if (!$HA_EXAMPLE_SO) {
  skip Needs ha_example plugin;
}

select PLUGIN_NAME,PLUGIN_STATUS,PLUGIN_TYPE from information_schema.plugins where plugin_library like 'ha_example%';
INSTALL PLUGIN IF NOT EXISTS example SONAME 'ha_example';
select PLUGIN_NAME,PLUGIN_STATUS,PLUGIN_TYPE from information_schema.plugins where plugin_library like 'ha_example%';

--replace_regex /\.dll/.so/
--error ER_PLUGIN_INSTALLED
INSTALL PLUGIN example SONAME 'ha_example';

INSTALL PLUGIN IF NOT EXISTS example SONAME 'ha_example';
SHOW WARNINGS;

UNINSTALL PLUGIN IF EXISTS example;
select PLUGIN_NAME,PLUGIN_STATUS,PLUGIN_TYPE from information_schema.plugins where plugin_library like 'ha_example%';

UNINSTALL PLUGIN IF EXISTS example;
SHOW WARNINGS;

--error ER_SP_DOES_NOT_EXIST
UNINSTALL PLUGIN example;

INSTALL SONAME 'ha_example';
select PLUGIN_NAME,PLUGIN_STATUS,PLUGIN_TYPE from information_schema.plugins where plugin_library like 'ha_example%';

UNINSTALL SONAME IF EXISTS 'ha_example';

--replace_regex /\.dll/.so/
UNINSTALL SONAME IF EXISTS 'ha_example';
--replace_regex /\.dll/.so/
SHOW WARNINGS;
select PLUGIN_NAME,PLUGIN_STATUS,PLUGIN_TYPE from information_schema.plugins where plugin_library like 'ha_example%';

--replace_regex /\.dll/.so/
--error ER_SP_DOES_NOT_EXIST
UNINSTALL SONAME 'ha_example';
