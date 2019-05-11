/* Copyright (c) 2014, Sergei Golubchik and MariaDB

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1335  USA */

#include <my_config.h>
#include <mysql/plugin_password_validation.h>
#include <crack.h>
#include <string.h>
#include <alloca.h>
#include <my_sys.h>
#include <mysqld_error.h>

static char *dictionary;

static int crackme(MYSQL_LEX_STRING *username, MYSQL_LEX_STRING *password)
{
  char *user= alloca(username->length + 1);
  char *host;
  const char *res;

  memcpy(user, username->str, username->length);
  user[username->length]= 0;
  if ((host= strchr(user, '@')))
    *host++= 0;

  if ((res= FascistCheckUser(password->str, dictionary, user, host)))
  {
    my_printf_error(ER_NOT_VALID_PASSWORD, "cracklib: %s",
                    MYF(ME_JUST_WARNING), res);
    return TRUE;
  }

  return FALSE;
}

static MYSQL_SYSVAR_STR(dictionary, dictionary, PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY,
  "Path to a cracklib dictionary", NULL, NULL, 0);

/* optional user-friendly nicety */
void set_default_dictionary_path() __attribute__((constructor));
void set_default_dictionary_path()
{
  MYSQL_SYSVAR_NAME(dictionary).def_val = GetDefaultCracklibDict();
}

static struct st_mysql_sys_var* sysvars[]= {
  MYSQL_SYSVAR(dictionary),
  NULL
};

static struct st_mariadb_password_validation info=
{
  MariaDB_PASSWORD_VALIDATION_INTERFACE_VERSION,
  crackme
};

maria_declare_plugin(cracklib_password_check)
{
  MariaDB_PASSWORD_VALIDATION_PLUGIN,
  &info,
  "cracklib_password_check",
  "Sergei Golubchik",
  "Password validation via CrackLib",
  PLUGIN_LICENSE_GPL,
  NULL,
  NULL,
  0x0100,
  NULL,
  sysvars,
  "1.0",
  MariaDB_PLUGIN_MATURITY_STABLE
}
maria_declare_plugin_end;
