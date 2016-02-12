#include "config.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "sql_plugin.h"
#include "handler.h"

#include "plugin_api.h"

extern ib_cb_t* innodb_api_cb;

static my_bool get_innodb_cb(THD *unused, plugin_ref plugin, void *arg)
{
	ib_cb_t*** innodb_cb_ptr = (ib_cb_t***) arg;

	if (strcmp(plugin_name(plugin)->str, "InnoDB") == 0)
	{
		if (plugin_dlib(plugin) != NULL)
		{
			*innodb_cb_ptr = (ib_cb_t**) dlsym(plugin_dlib(plugin)->handle, "innodb_api_cb");
		}
		else
		{
			*innodb_cb_ptr = &innodb_api_cb;
		}
		return 1;
	}
	return 0;
}

ib_cb_t** obtain_innodb_cb()
{
	ib_cb_t** innodb_cb = NULL;

	plugin_foreach(NULL, get_innodb_cb,
		       MYSQL_STORAGE_ENGINE_PLUGIN, &innodb_cb);

	return innodb_cb;
}
