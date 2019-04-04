let $VERSION_COMPILE_OS_WIN=
  `SELECT IF(@@version_compile_os like 'Win%', 1, 0)`;
if ($VERSION_COMPILE_OS_WIN)
{
  INSTALL PLUGIN vp SONAME 'ha_vp.dll';
  CREATE FUNCTION vp_copy_tables RETURNS INT SONAME 'ha_vp.dll';
}
if (!$VERSION_COMPILE_OS_WIN)
{
  INSTALL PLUGIN vp SONAME 'ha_vp.so';
  CREATE FUNCTION vp_copy_tables RETURNS INT SONAME 'ha_vp.so';
}

let $PLUGIN_VERSION=
  `SELECT SUBSTRING_INDEX(plugin_version, '.', 1)
     FROM information_schema.plugins
    WHERE plugin_name = 'VP'`;
