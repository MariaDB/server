/* Copyright (c) 2021, Oleksandr Byelkin and MariaDB

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

#include <stdio.h>     // for snprintf
#include <string.h>    // for memset
#include <mysql/plugin_password_validation.h>
#include <mysqld_error.h>

#define HISTORY_DB_NAME "password_reuse_check_history"

#define SQL_BUFF_LEN 2048

#define STRING_WITH_LEN(X) (X), ((size_t) (sizeof(X) - 1))

// 0 - unlimited, otherwise number of days to check
static unsigned interval= 0;

// helping string for bin_to_hex512
static char digits[]= "0123456789ABCDEF";


/**
  Convert string of 512 bits (64 bytes) to hex representation

  @param to              pointer to the result puffer
                         (should be at least 64*2 bytes)
  @param str             pointer to 512 bits (64 bytes string)
*/

static void bin_to_hex512(char *to, const unsigned char *str)
{
  const unsigned char *str_end= str + (512/8);
  for (; str != str_end; ++str)
  {
    *to++= digits[((unsigned char) *str) >> 4];
    *to++= digits[((unsigned char) *str) & 0x0F];
  }
}


/**
  Send SQL error as ER_UNKNOWN_ERROR for information

  @param mysql           Connection handler
*/

static void report_sql_error(MYSQL *mysql)
{
  my_printf_error(ER_UNKNOWN_ERROR, "password_reuse_check:[%d] %s", ME_WARNING,
                  mysql_errno(mysql), mysql_error(mysql));
}


/**
  Create the history of passwords table for this plugin.

  @param mysql           Connection handler

  @retval 1 - Error
  @retval 0 - OK
*/

static int create_table(MYSQL *mysql)
{
  if (mysql_real_query(mysql,
        // 512/8 = 64
        STRING_WITH_LEN("CREATE TABLE mysql." HISTORY_DB_NAME
                        " ( hash binary(64),"
                        " time timestamp default current_timestamp,"
                        " primary key (hash), index tm (time) )"
                        " ENGINE=Aria")))
  {
    report_sql_error(mysql);
    return 1;
  }
  return 0;
}


/**
  Run this query and create table if needed

  @param mysql           Connection handler
  @param query           The query to run
  @param len             length of the query text

  @retval 1 - Error
  @retval 0 - OK
*/

static int run_query_with_table_creation(MYSQL *mysql, const char *query,
                                         size_t len)
{
  if (mysql_real_query(mysql, query, len))
  {
    unsigned int rc= mysql_errno(mysql);
    if (rc != ER_NO_SUCH_TABLE)
    {
      // suppress this error in case of try to add the same password twice
      if (rc != ER_DUP_ENTRY)
        report_sql_error(mysql);
      return 1;
    }
    if (create_table(mysql))
      return 1;
    if (mysql_real_query(mysql, query, len))
    {
      report_sql_error(mysql);
      return 1;
    }
  }
  return 0;
}


/**
  Password validator

  @param username        User name (part of whole login name)
  @param password        Password to validate
  @param hostname        Host name (part of whole login name)

  @retval 1 - Password is not OK or an error happened
  @retval 0 - Password is OK
*/

static int validate(const MYSQL_CONST_LEX_STRING *username,
                    const MYSQL_CONST_LEX_STRING *password,
                    const MYSQL_CONST_LEX_STRING *hostname)
{
  MYSQL *mysql= NULL;
  size_t key_len= username->length + password->length + hostname->length;
  size_t buff_len= (key_len > SQL_BUFF_LEN ? key_len : SQL_BUFF_LEN);
  size_t len;
  char *buff= malloc(buff_len);
  unsigned char hash[512/8];
  char escaped_hash[512/8*2 + 1];
  if (!buff)
    return 1;

  mysql= mysql_init(NULL);
  if (!mysql)
  {
    free(buff);
    return 1;
  }

  memcpy(buff, hostname->str, hostname->length);
  memcpy(buff + hostname->length, username->str, username->length);
  memcpy(buff + hostname->length + username->length, password->str,
          password->length);
  buff[key_len]= 0;
  memset(hash, 0, sizeof(hash));
  my_sha512(hash, buff, key_len);
  if (mysql_real_connect_local(mysql) == NULL)
    goto sql_error;

  if (interval)
  {
    // trim the table
    len= snprintf(buff, buff_len,
                  "DELETE FROM mysql." HISTORY_DB_NAME
                  " WHERE time < DATE_SUB(NOW(), interval %d day)",
                  interval);
    if (run_query_with_table_creation(mysql, buff, len))
      goto sql_error;
  }

  bin_to_hex512(escaped_hash, hash);
  escaped_hash[512/8*2]= '\0';
  len= snprintf(buff, buff_len,
                "INSERT INTO mysql." HISTORY_DB_NAME "(hash) "
                "values (x'%s')",
                escaped_hash);
  if (run_query_with_table_creation(mysql, buff, len))
    goto sql_error;

  free(buff);
  mysql_close(mysql);
  return 0; // OK

sql_error:
  free(buff);
  if (mysql)
    mysql_close(mysql);
  return 1; // Error
}

static MYSQL_SYSVAR_UINT(interval, interval, PLUGIN_VAR_RQCMDARG,
  "Password history retention period in days (0 means unlimited)", NULL, NULL,
  0, 0, 365*100, 1);


static struct st_mysql_sys_var* sysvars[]= {
  MYSQL_SYSVAR(interval),
  NULL
};

static struct st_mariadb_password_validation info=
{
  MariaDB_PASSWORD_VALIDATION_INTERFACE_VERSION,
  validate
};

maria_declare_plugin(password_reuse_check)
{
  MariaDB_PASSWORD_VALIDATION_PLUGIN,
  &info,
  "password_reuse_check",
  "Oleksandr Byelkin",
  "Prevent password reuse",
  PLUGIN_LICENSE_GPL,
  NULL,
  NULL,
  0x0100,
  NULL,
  sysvars,
  "1.0",
  MariaDB_PLUGIN_MATURITY_ALPHA
}
maria_declare_plugin_end;
