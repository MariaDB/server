/***********************************************************************

Copyright (c) 2011, 2014, Oracle and/or its affiliates. All rights reserved.

This program is free software; you can redistribute it and/or modify it
under the terms of the GNU General Public License as published by the
Free Software Foundation; version 2 of the License.

This program is distributed in the hope that it will be useful, but
WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General
Public License for more details.

You should have received a copy of the GNU General Public License along
with this program; if not, write to the Free Software Foundation, Inc.,
51 Franklin Street, Suite 500, Boston, MA 02110-1335 USA

***********************************************************************/

#include "config.h"

#include "memcached_mysql.h"
#include <stdlib.h>
#include <ctype.h>
#include <mysql_version.h>
#include "sql_plugin.h"
#include "log.h"

/** Variables for configure options */
static char *mci_engine_lib_name = NULL;
static char *mci_engine_lib_path = NULL;
static char *mci_option = NULL;
static unsigned int mci_r_batch_size = 1;
static unsigned int mci_w_batch_size = 1;
static my_bool mci_enable_binlog = false;

static MYSQL_SYSVAR_STR(engine_lib_name, mci_engine_lib_name,
                        PLUGIN_VAR_READONLY | PLUGIN_VAR_MEMALLOC,
                        "memcached engine library name", NULL, NULL,
                        "daemon_memcached_engine_ib.so");

static MYSQL_SYSVAR_STR(engine_lib_path, mci_engine_lib_path,
                        PLUGIN_VAR_READONLY | PLUGIN_VAR_MEMALLOC,
                        "memcached engine library path", NULL, NULL, NULL);

static MYSQL_SYSVAR_STR(option, mci_option,
                        PLUGIN_VAR_READONLY | PLUGIN_VAR_MEMALLOC,
                        "memcached option string", NULL, NULL, NULL);

static MYSQL_SYSVAR_UINT(r_batch_size, mci_r_batch_size,
                         PLUGIN_VAR_READONLY,
                         "read batch commit size", 0, 0, 1,
                         1, 1073741824, 0);

static MYSQL_SYSVAR_UINT(w_batch_size, mci_w_batch_size,
                         PLUGIN_VAR_READONLY,
                         "write batch commit size", 0, 0, 1,
                         1, 1048576, 0);

static MYSQL_SYSVAR_BOOL(enable_binlog, mci_enable_binlog,
                         PLUGIN_VAR_READONLY,
                         "whether to enable binlog",
                         NULL, NULL, FALSE);

static struct st_mysql_sys_var *daemon_memcached_sys_var[] = {
    MYSQL_SYSVAR(engine_lib_name),
    MYSQL_SYSVAR(engine_lib_path),
    MYSQL_SYSVAR(option),
    MYSQL_SYSVAR(r_batch_size),
    MYSQL_SYSVAR(w_batch_size),
    MYSQL_SYSVAR(enable_binlog),
    0
};

static int daemon_memcached_plugin_deinit(void *p)
{
    struct st_plugin_int *plugin = (struct st_plugin_int *) p;
    memcached_context_t *context = NULL;
    int loop_count = 0;
    unsigned int i;

    /* If memcached plugin is still initializing, wait for a
    while.*/
    while (!init_complete() && loop_count < 15000) {
        usleep(1000);
        loop_count++;
    }

    if (!init_complete()) {
        sql_print_warning("Plugin daemon_memcached: %s", "Memcached plugin is still"
                          " initializing. Can't shut down it.\n");
        return (0);
    }

    loop_count = 0;
    if (!shutdown_complete()) {
        shutdown_server();
    }

    loop_count = 0;

    while (!shutdown_complete() && loop_count < 50000) {
        usleep(1000);
        loop_count++;
    }

    if (!shutdown_complete()) {
        sql_print_warning("Plugin daemon_memcached: %s", "Waited for 50 seconds"
                          " for memcached thread to exit. Now force terminating"
                          " the thread.\n");
    }

    context = (memcached_context_t *) (plugin->data);

    pthread_cancel(context->thread);

    if (context->config.engine_library) {
        my_free(context->config.engine_library);
    }

    for (i = 0; i < context->containers_number; i++) {
        free(context->containers[i].name);
    }

    if (context->containers) {
        free(context->containers);
    }

    my_free(context);

    return (0);
}

static int daemon_memcached_plugin_init(void *p)
{
    memcached_context_t *context;
    pthread_attr_t attr;
    struct st_plugin_int *plugin = (struct st_plugin_int *) p;

    context = (memcached_context_t *) my_malloc(/* 5.7: PSI_INSTRUMENT_ME, */
                                                sizeof(*context), MYF(0));

    if (mci_engine_lib_name) {
        char *lib_path = (mci_engine_lib_path)
                         ? mci_engine_lib_path : opt_plugin_dir;
        int lib_len = strlen(lib_path)
                      + strlen(mci_engine_lib_name)
                      + strlen(FN_DIRSEP) + 1;

        context->config.engine_library = (char *) my_malloc(/* 5.7: PSI_INSTRUMENT_ME, */
                                                                  lib_len, MYF(0));

        strxmov(context->config.engine_library, lib_path,
                FN_DIRSEP, mci_engine_lib_name, NullS);
    } else {
        context->config.engine_library = NULL;
    }

    context->config.option = mci_option;
    context->config.r_batch_size = mci_r_batch_size;
    context->config.w_batch_size = mci_w_batch_size;
    context->config.enable_binlog = mci_enable_binlog;

    context->containers = NULL;
    context->containers_number = 0;

    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_JOINABLE);

    /* now create the thread */
    if (pthread_create(&context->thread, &attr,
                       daemon_memcached_main,
                       context) != 0) {
        sql_print_warning("Plugin daemon_memcached: %s", "Could not create memcached daemon thread!\n");
        exit(0);
    }

    plugin->data = (void *) context;

    return (0);
}

struct st_mysql_daemon daemon_memcached_plugin = {MYSQL_DAEMON_INTERFACE_VERSION};

maria_declare_plugin(daemon_memcached)
{
    MYSQL_DAEMON_PLUGIN,
    &daemon_memcached_plugin,
    "daemon_memcached",
    "Oracle Corporation",
    "Memcached Daemon",
    PLUGIN_LICENSE_GPL,
    daemon_memcached_plugin_init,
    daemon_memcached_plugin_deinit,
    0x0100,
    NULL,
    daemon_memcached_sys_var,
    "1.0",
    MariaDB_PLUGIN_MATURITY_EXPERIMENTAL
}
maria_declare_plugin_end;
