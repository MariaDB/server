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
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1335  USA */

#include <mysqld_error.h>
#include <my_attribute.h>
#include <mysql/plugin_password_validation.h>
#include <ctype.h>
#include <string.h>

static unsigned min_length, min_digits, min_letters, min_others;

#define SQL_BUFF_LEN 2048
#define USERNAME_VIEW_NAME "username_view"
#define STRING_WITH_LEN(X) (X), ((size_t) (sizeof(X) - 1))
MYSQL *mysql= NULL;

static int check_password_exists(MYSQL *mysql, const char *query,
                                 size_t len)
{
  if (mysql_real_query(mysql, query, len)) {
    return 1;
  }

  MYSQL_RES *result = mysql_store_result(mysql);
  if (result)
  {
    my_printf_error(ER_NOT_VALID_PASSWORD,
                    "simple_password_check: The password equal to some user name",
                    ME_WARNING);
    return 1;
  }
  else
    return 0;
}

static int init(void* h)
{
  mysql= mysql_init(NULL);
  if (!mysql)
    return 1;
  if (mysql_real_connect_local(mysql) == NULL)
    return 1;

  if (mysql_real_query(mysql, STRING_WITH_LEN("CREATE VIEW mysql."
                                              USERNAME_VIEW_NAME
                                              " AS SELECT DISTINCT user FROM mysql.user")))
  {

    my_printf_error(ER_UNKNOWN_ERROR, "simple_password_check:[%d] %s", ME_WARNING,
                    mysql_errno(mysql), mysql_error(mysql));
    return 1;
  }

  mysql_close(mysql);
  return 0;
}

static int deinit(void *h)
{
  return 1; /* don't unload me */
}

static int validate(const MYSQL_CONST_LEX_STRING *username,
                    const MYSQL_CONST_LEX_STRING *password,
                    const MYSQL_CONST_LEX_STRING *hostname
                      __attribute__((unused)))
{
  unsigned digits=0 , uppers=0 , lowers=0, others=0, length= (unsigned)password->length;
  const char *ptr= password->str, *end= ptr + length;

  size_t len;

  char *buff= malloc(SQL_BUFF_LEN);
  if (!buff)
    return 1;

  if (mysql_real_connect_local(mysql) == NULL)
    goto sql_error;

  buff[24]= 0;

  len= snprintf(buff, SQL_BUFF_LEN,
                "SELECT 1 FROM mysql."
                USERNAME_VIEW_NAME
                " WHERE user = '%s'", ptr
  );
  if (check_password_exists(mysql, buff, len))
    goto sql_error;

  free(buff);
  mysql_close(mysql);

  if (strncmp(password->str, username->str, length) == 0)
  {
    // warning used to do not change error code
    my_printf_error(ER_NOT_VALID_PASSWORD,
                    "simple_password_check: The password equal to the user name",
                    ME_WARNING);
    return 1;
  }

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

  // warnings used to do not change error code
  if (length < min_length)
    my_printf_error(ER_NOT_VALID_PASSWORD,
                    "simple_password_check: Too short password (< %u)",
                    ME_WARNING, min_length);
  if (uppers < min_letters)
    my_printf_error(ER_NOT_VALID_PASSWORD,
                    "simple_password_check: Not enough upper case "
                    "letters (< %u)",ME_WARNING,  min_letters);
  if (lowers < min_letters)
    my_printf_error(ER_NOT_VALID_PASSWORD,
                    "simple_password_check: Not enough lower case "
                    "letters (< %u)",ME_WARNING,  min_letters);
  if (digits < min_digits)
    my_printf_error(ER_NOT_VALID_PASSWORD,
                    "simple_password_check: Not enough digits (< %u)",
                    ME_WARNING, min_digits);
  if (others < min_others)
    my_printf_error(ER_NOT_VALID_PASSWORD,
                    "simple_password_check: Not enough special "
                    "characters (< %u)",ME_WARNING,  min_others);
  /* remember TRUE means the password failed the validation */
  return length < min_length  ||
         uppers < min_letters ||
         lowers < min_letters ||
         digits < min_digits  ||
         others < min_others;

sql_error:
  free(buff);
  if (mysql)
    mysql_close(mysql);
  return 1; // Error
}

static void fix_min_length(MYSQL_THD thd __attribute__((unused)),
                           struct st_mysql_sys_var *var
                           __attribute__((unused)),
                           void *var_ptr, const void *save)
{
  unsigned int new_min_length;
  *((unsigned int *)var_ptr)= *((unsigned int *)save);
  new_min_length= min_digits + 2 * min_letters + min_others;
  if (min_length < new_min_length)
  {
    my_printf_error(ER_TRUNCATED_WRONG_VALUE,
                    "Adjusted the value of simple_password_check_minimal_length "
                    "from %u to %u", ME_WARNING, min_length, new_min_length);
    min_length= new_min_length;
  }
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
  MariaDB_PLUGIN_MATURITY_STABLE
}
maria_declare_plugin_end;
