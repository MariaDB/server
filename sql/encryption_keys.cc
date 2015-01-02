#include <my_global.h>
#include <mysql/plugin_encryption_key_management.h>
#include "encryption_keys.h"
#include "log.h"
#include "sql_plugin.h"

#ifndef DBUG_OFF
my_bool debug_use_static_encryption_keys = 0;
uint opt_debug_encryption_key_version = 0;
#endif

/* there can be only one encryption key management plugin enabled */
static plugin_ref encryption_key_manager= 0;
static struct st_mariadb_encryption_key_management *handle;

uint get_latest_encryption_key_version()
{
#ifndef DBUG_OFF
  if (debug_use_static_encryption_keys)
  {
    //mysql_mutex_lock(&LOCK_global_system_variables);
    uint res = opt_debug_encryption_key_version;
    //mysql_mutex_unlock(&LOCK_global_system_variables);
    return res;
  }
#endif

  if (encryption_key_manager)
    return handle->get_latest_key_version();

  return BAD_ENCRYPTION_KEY_VERSION;
}

uint has_encryption_key(uint version)
{
  if (encryption_key_manager)
    return handle->has_key_version(version);

  return 0;
}

uint get_encryption_key_size(uint version)
{
  if (encryption_key_manager)
    return handle->get_key_size(version);

  return 0;
}

int get_encryption_key(uint version, uchar* key, uint size)
{
#ifndef DBUG_OFF
  if (debug_use_static_encryption_keys)
  {
    memset(key, 0, size);
    // Just don't support tiny keys, no point anyway.
    if (size < 4)
      return 1;

    mi_int4store(key, version);
    return 0;
  }
#endif

  if (encryption_key_manager)
    return handle->get_key(version, key, size);

  return 1;
}

int get_encryption_iv(uint version, uchar* iv, uint size)
{
  if (encryption_key_manager)
    return handle->get_iv(version, iv, size);

  return 1;
}

int initialize_encryption_key_management_plugin(st_plugin_int *plugin)
{
  if (encryption_key_manager)
    return 1;

  if (plugin->plugin->init && plugin->plugin->init(plugin))
  {
    sql_print_error("Plugin '%s' init function returned error.",
                    plugin->name.str);
    return 1;
  }

  encryption_key_manager= plugin_lock(NULL, plugin_int_to_ref(plugin));
  handle= (struct st_mariadb_encryption_key_management*)
            plugin->plugin->info;
  return 0;
}

int finalize_encryption_key_management_plugin(st_plugin_int *plugin)
{
  DBUG_ASSERT(encryption_key_manager);

  if (plugin->plugin->deinit && plugin->plugin->deinit(NULL))
  {
    DBUG_PRINT("warning", ("Plugin '%s' deinit function returned error.",
                           plugin->name.str));
  }
  plugin_unlock(NULL, encryption_key_manager);
  encryption_key_manager= 0;
  return 0;
}

