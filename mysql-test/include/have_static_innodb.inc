source include/have_innodb.inc;

if (!`select count(*) from information_schema.plugins
         where plugin_name = 'innodb' and plugin_status = 'active' and
              plugin_library is null`) {
  skip Need compiled-in InnoDB;
}
