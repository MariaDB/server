/* Copyright (c) 2014, Sergei Golubchik and MariaDB
   Copyright (c) 2012, 2013, Oracle and/or its affiliates.

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

#include <mysql/plugin_password_validation.h>
#include <ctype.h>
#include <string.h>
#include <my_attribute.h>

static unsigned min_length, min_digits, min_letters, min_others;

static int validate(MYSQL_LEX_STRING *username, MYSQL_LEX_STRING *password)
{
  unsigned digits=0 , uppers=0 , lowers=0, others=0, length= password->length;
  const char *ptr= password->str, *end= ptr + length;

  if (strncmp(password->str, username->str, length) == 0)
    return 1;

  /* everything non-ascii is the "other" character and is good for the password */
  for(; ptr < end; ptr++)
  {
    if (isdigit(*ptr))
      digits++;
    else if (isupper(*ptr))
      uppers++;
    else if (islower(*ptr))
      lowers++;
    else
      others++;
  }
  /* remember TRUE means the password failed the validation */
  return length < min_length  ||
         uppers < min_letters ||
         lowers < min_letters ||
         digits < min_digits  ||
         others < min_others;
}

static void fix_min_length(MYSQL_THD thd __attribute__((unused)),
                           struct st_mysql_sys_var *var,
                           void *var_ptr, const void *save)
{
  *((unsigned int *)var_ptr)= *((unsigned int *)save);
  if (min_length < min_digits + 2 * min_letters + min_others)
    min_length= min_digits + 2 * min_letters + min_others;
}

static MYSQL_SYSVAR_UINT(minimal_length, min_length, PLUGIN_VAR_RQCMDARG,
  "Minimal required password length", NULL, fix_min_length, 8, 0, 1000, 1);

static MYSQL_SYSVAR_UINT(digits, min_digits, PLUGIN_VAR_RQCMDARG,
  "Minimal required number of digits", NULL, fix_min_length, 1, 0, 1000, 1);

static MYSQL_SYSVAR_UINT(letters_same_case, min_letters, PLUGIN_VAR_RQCMDARG,
  "Minimal required number of letters of the same letter case."
  "This limit is applied separately to upper-case and lower-case letters",
  NULL, fix_min_length, 1, 0, 1000, 1);

static MYSQL_SYSVAR_UINT(other_characters, min_others, PLUGIN_VAR_RQCMDARG,
  "Minimal required number of other (not letters or digits) characters",
  NULL, fix_min_length, 1, 0, 1000, 1);

static struct st_mysql_sys_var* sysvars[]= {
  MYSQL_SYSVAR(minimal_length),
  MYSQL_SYSVAR(digits),
  MYSQL_SYSVAR(letters_same_case),
  MYSQL_SYSVAR(other_characters),
  NULL
};

static struct st_mariadb_password_validation info=
{
  MariaDB_PASSWORD_VALIDATION_INTERFACE_VERSION,
  validate
};

maria_declare_plugin(simple_password_check)
{
  MariaDB_PASSWORD_VALIDATION_PLUGIN,
  &info,
  "simple_password_check",
  "Sergei Golubchik",
  "Simple password strength checks",
  PLUGIN_LICENSE_GPL,
  NULL,
  NULL,
  0x0100,
  NULL,
  sysvars,
  "1.0",
  MariaDB_PLUGIN_MATURITY_GAMMA
}
maria_declare_plugin_end;
