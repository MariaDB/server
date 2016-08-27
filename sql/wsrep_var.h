/* Copyright (C) 2013 Codership Oy <info@codership.com>

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License along
   with this program; if not, write to the Free Software Foundation, Inc.,
   51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA. */

#include <my_config.h>

#ifndef WSREP_VAR_H
#define WSREP_VAR_H

#ifdef WITH_WSREP

#define WSREP_CLUSTER_NAME        "my_wsrep_cluster"
#define WSREP_NODE_INCOMING_AUTO  "AUTO"
#define WSREP_START_POSITION_ZERO "00000000-0000-0000-0000-000000000000:-1"

// MySQL variables funcs

#include "sql_priv.h"
#include <sql_plugin.h>
#include <mysql/plugin.h>

class sys_var;
class set_var;
class THD;

int wsrep_init_vars();

#define CHECK_ARGS   (sys_var *self, THD* thd, set_var *var)
#define UPDATE_ARGS  (sys_var *self, THD* thd, enum_var_type type)
#define DEFAULT_ARGS (THD* thd, enum_var_type var_type)
#define INIT_ARGS    (const char* opt)

struct system_variables;
bool wsrep_causal_reads_update(struct system_variables *sv);

extern bool wsrep_on_update                  UPDATE_ARGS;
extern bool wsrep_sync_wait_update           UPDATE_ARGS;
extern bool wsrep_start_position_check       CHECK_ARGS;
extern bool wsrep_start_position_update      UPDATE_ARGS;
extern bool wsrep_start_position_init        INIT_ARGS;

extern bool wsrep_provider_check             CHECK_ARGS;
extern bool wsrep_provider_update            UPDATE_ARGS;
extern void wsrep_provider_init              INIT_ARGS;

extern bool wsrep_provider_options_check     CHECK_ARGS;
extern bool wsrep_provider_options_update    UPDATE_ARGS;
extern void wsrep_provider_options_init      INIT_ARGS;

extern bool wsrep_cluster_address_check      CHECK_ARGS;
extern bool wsrep_cluster_address_update     UPDATE_ARGS;
extern void wsrep_cluster_address_init       INIT_ARGS;

extern bool wsrep_cluster_name_check         CHECK_ARGS;
extern bool wsrep_cluster_name_update        UPDATE_ARGS;

extern bool wsrep_node_name_check            CHECK_ARGS;
extern bool wsrep_node_name_update           UPDATE_ARGS;

extern bool wsrep_node_address_check         CHECK_ARGS;
extern bool wsrep_node_address_update        UPDATE_ARGS;
extern void wsrep_node_address_init          INIT_ARGS;

extern bool wsrep_sst_method_check           CHECK_ARGS;
extern bool wsrep_sst_method_update          UPDATE_ARGS;
extern void wsrep_sst_method_init            INIT_ARGS;

extern bool wsrep_sst_receive_address_check  CHECK_ARGS;
extern bool wsrep_sst_receive_address_update UPDATE_ARGS;

extern bool wsrep_sst_auth_check             CHECK_ARGS;
extern bool wsrep_sst_auth_update            UPDATE_ARGS;

extern bool wsrep_sst_donor_check            CHECK_ARGS;
extern bool wsrep_sst_donor_update           UPDATE_ARGS;

extern bool wsrep_slave_threads_check        CHECK_ARGS;
extern bool wsrep_slave_threads_update       UPDATE_ARGS;

extern bool wsrep_desync_check               CHECK_ARGS;
extern bool wsrep_desync_update              UPDATE_ARGS;

#else  /* WITH_WSREP */

#define WSREP_NONE
#define wsrep_provider_init(X)
#define wsrep_init_vars() (0)
#define wsrep_start_position_init(X)

#endif /* WITH_WSREP */
#endif /* WSREP_VAR_H */
