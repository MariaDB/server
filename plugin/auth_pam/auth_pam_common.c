/*
   Copyright (c) 2011, 2018 MariaDB Corporation

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02111-1301 USA */

/*
  In this file we gather the plugin interface definitions
  that are same in all the PAM plugin versions.
  To be included into auth_pam.c and auth_pam_v1.c.
*/

static struct st_mysql_auth info =
{
  MYSQL_AUTHENTICATION_INTERFACE_VERSION,
  "dialog",
  pam_auth,
  NULL, NULL /* no PASSWORD() */
};

static char use_cleartext_plugin;
static MYSQL_SYSVAR_BOOL(use_cleartext_plugin, use_cleartext_plugin,
       PLUGIN_VAR_NOCMDARG | PLUGIN_VAR_READONLY,
       "Use mysql_cleartext_plugin on the client side instead of the dialog "
       "plugin. This may be needed for compatibility reasons, but it only "
       "supports simple PAM policies that don't require anything besides "
       "a password", NULL, NULL, 0);

static MYSQL_SYSVAR_BOOL(winbind_workaround, winbind_hack, PLUGIN_VAR_OPCMDARG,
       "Compare usernames case insensitively to work around pam_winbind "
       "unconditional username lowercasing", NULL, NULL, 0);

#ifndef DBUG_OFF
static MYSQL_SYSVAR_BOOL(debug, pam_debug, PLUGIN_VAR_OPCMDARG,
       "Log all PAM activity", NULL, NULL, 0);
#endif


static struct st_mysql_sys_var* vars[] = {
  MYSQL_SYSVAR(use_cleartext_plugin),
  MYSQL_SYSVAR(winbind_workaround),
#ifndef DBUG_OFF
  MYSQL_SYSVAR(debug),
#endif
  NULL
};
