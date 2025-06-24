/* Copyright 2022 Codership Oy <info@codership.com>

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA */

#ifndef WSREP_PLUGIN_H
#define WSREP_PLUGIN_H

class option;
struct st_mysql_sys_var;

/* Returns true if provider plugin was initialized and is active */
bool wsrep_provider_plugin_enabled();

/* Set the given sysvars array for provider plugin.
   Must be called before the plugin is initialized. */
void wsrep_provider_plugin_set_sysvars(st_mysql_sys_var **);

/* Construct a sysvar corresponding to the given provider option */
struct st_mysql_sys_var *
wsrep_make_sysvar_for_option(wsrep::provider_options::option *);

/* Destroy a sysvar created by make_sysvar_for_option */
void wsrep_destroy_sysvar(struct st_mysql_sys_var *);

#endif /* WSREP_PLUGIN_H */
