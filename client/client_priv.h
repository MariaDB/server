/*
   Copyright (c) 2001, 2012, Oracle and/or its affiliates.
   Copyright (c) 2009, 2024, MariaDB

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1335  USA
*/

/* Common defines for all clients */

#include <my_global.h>
#include <my_sys.h>
#include <m_string.h>
#include <mysql.h>
#include <errmsg.h>
#include <my_getopt.h>
#include <mysql_version.h>

#ifndef WEXITSTATUS
# ifdef __WIN__
#  define WEXITSTATUS(stat_val) (stat_val)
# else
#  define WEXITSTATUS(stat_val) ((unsigned)(stat_val) >> 8)
# endif
#endif

enum options_client
{
  OPT_CHARSETS_DIR=256, OPT_DEFAULT_CHARSET,
  OPT_PAGER, OPT_TEE,
  OPT_OPTIMIZE,
  OPT_TABLES,
  OPT_MASTER_DATA,
  OPT_SSL_KEY, OPT_SSL_CERT, OPT_SSL_CA, OPT_SSL_CAPATH,
  OPT_SSL_CIPHER, OPT_LOCAL_INFILE,
  OPT_COMPACT,
  OPT_MYSQL_PROTOCOL,
  OPT_SKIP_OPTIMIZATION,
  OPT_COMPATIBLE, OPT_DELIMITER,
  OPT_SERVER_ARG,
  OPT_START_DATETIME, OPT_STOP_DATETIME,
  OPT_IGNORE_DATABASE,
  OPT_IGNORE_TABLE,
  OPT_MYSQLDUMP_SLAVE_DATA,
  OPT_SLAP_CSV,
  OPT_BASE64_OUTPUT_MODE,
  OPT_FIX_TABLE_NAMES, OPT_FIX_DB_NAMES,
  OPT_WRITE_BINLOG,
  OPT_PLUGIN_DIR,
  OPT_DEFAULT_AUTH,
  OPT_REWRITE_DB,
  OPT_SSL_CRL, OPT_SSL_CRLPATH,
  OPT_IGNORE_DATA,
  OPT_PRINT_ROW_COUNT, OPT_PRINT_ROW_EVENT_POSITIONS,
  OPT_CHECK_IF_UPGRADE_NEEDED,
  OPT_COMPATIBILTY_CLEARTEXT_PLUGIN,
  OPT_MAX_CLIENT_OPTION /* should be always the last */
};

/**
  First mysql version supporting the information schema.
*/
#define FIRST_INFORMATION_SCHEMA_VERSION 50003

/**
  Name of the information schema database.
*/
#define INFORMATION_SCHEMA_DB_NAME "information_schema"

/**
  First mysql version supporting the performance schema.
*/
#define FIRST_PERFORMANCE_SCHEMA_VERSION 50503

/**
  Name of the performance schema database.
*/
#define PERFORMANCE_SCHEMA_DB_NAME "performance_schema"
