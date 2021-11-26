/* Copyright (C) 2013-2021 Codership Oy <info@codership.com>

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License along
   with this program; if not, write to the Free Software Foundation, Inc.,
   51 Franklin Street, Fifth Floor, Boston, MA 02110-1335 USA. */

#ifndef WSREP_VAR_H
#define WSREP_VAR_H

#include <my_config.h>

#ifdef WITH_WSREP

#define WSREP_CLUSTER_NAME              "my_wsrep_cluster"
#define WSREP_NODE_INCOMING_AUTO        "AUTO"
#define WSREP_START_POSITION_ZERO       "00000000-0000-0000-0000-000000000000:-1"
#define WSREP_START_POSITION_ZERO_GTID  "00000000-0000-0000-0000-000000000000:-1,0-0-0"

// MySQL variables funcs

#include "sql_priv.h"
#include <sql_plugin.h>
#include <mysql/plugin.h>

class sys_var;
class set_var;
class THD;

int wsrep_init_vars();
void wsrep_set_wsrep_on(THD *thd);

#define CHECK_ARGS   (sys_var *self, THD* thd, set_var *var)
#define UPDATE_ARGS  (sys_var *self, THD* thd, enum_var_type type)
#define DEFAULT_ARGS (THD* thd, enum_var_type var_type)
#define INIT_ARGS    (const char* opt)

extern bool wsrep_causal_reads_update        UPDATE_ARGS;
extern bool wsrep_on_check                   CHECK_ARGS;
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

extern bool wsrep_trx_fragment_size_check    CHECK_ARGS;
extern bool wsrep_trx_fragment_size_update   UPDATE_ARGS;

extern bool wsrep_trx_fragment_unit_update   UPDATE_ARGS;

extern bool wsrep_max_ws_size_check          CHECK_ARGS;
extern bool wsrep_max_ws_size_update         UPDATE_ARGS;

extern bool wsrep_reject_queries_update      UPDATE_ARGS;

extern bool wsrep_debug_update               UPDATE_ARGS;

extern bool wsrep_gtid_seq_no_check          CHECK_ARGS;

extern bool wsrep_gtid_domain_id_update      UPDATE_ARGS;

extern bool wsrep_mode_check                 CHECK_ARGS;
#else  /* WITH_WSREP */

#define wsrep_provider_init(X)
#define wsrep_init_vars() (0)
#define wsrep_start_position_init(X)

#endif /* WITH_WSREP */
#endif /* WSREP_VAR_H */
