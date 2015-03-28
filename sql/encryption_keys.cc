#include <my_global.h>
#include <mysql/plugin_encryption_key_management.h>
#include "log.h"
#include "sql_plugin.h"

/* there can be only one encryption key management plugin enabled */
static plugin_ref encryption_key_manager= 0;
static struct st_mariadb_encryption_key_management *handle;

unsigned int get_latest_encryption_key_version()
{
  if (encryption_key_manager)
    return handle->get_latest_key_version();

  return BAD_ENCRYPTION_KEY_VERSION;
}

unsigned int has_encryption_key(uint version)
{
  if (encryption_key_manager)
    return handle->has_key_version(version);

  return 0;
}

unsigned int get_encryption_key_size(uint version)
{
  if (encryption_key_manager)
    return handle->get_key_size(version);

  return 0;
}

int get_encryption_key(uint version, uchar* key, uint size)
{
  if (encryption_key_manager)
    return handle->get_key(version, key, size);

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
  if (plugin->plugin->deinit && plugin->plugin->deinit(NULL))
  {
    DBUG_PRINT("warning", ("Plugin '%s' deinit function returned error.",
                           plugin->name.str));
  }
  if (encryption_key_manager)
    plugin_unlock(NULL, encryption_key_manager);
  encryption_key_manager= 0;
  return 0;
}

