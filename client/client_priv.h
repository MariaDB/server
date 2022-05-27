/*
   Copyright (c) 2001, 2012, Oracle and/or its affiliates.
   Copyright (c) 2009, 2020, MariaDB

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
# ifdef _WIN32
#  define WEXITSTATUS(stat_val) (stat_val)
# else
#  define WEXITSTATUS(stat_val) ((unsigned)(stat_val) >> 8)
# endif
#endif

enum options_client
{
  OPT_CHARSETS_DIR=256, OPT_DEFAULT_CHARSET,
  OPT_PAGER, OPT_TEE,
  OPT_LOW_PRIORITY, OPT_AUTO_REPAIR, OPT_COMPRESS,
  OPT_DROP, OPT_LOCKS, OPT_KEYWORDS, OPT_DELAYED, OPT_OPTIMIZE,
  OPT_FTB, OPT_LTB, OPT_ENC, OPT_O_ENC, OPT_ESC, OPT_TABLES,
  OPT_MASTER_DATA, OPT_AUTOCOMMIT, OPT_AUTO_REHASH,
  OPT_LINE_NUMBERS, OPT_COLUMN_NAMES, OPT_CONNECT_TIMEOUT,
  OPT_MAX_ALLOWED_PACKET, OPT_NET_BUFFER_LENGTH,
  OPT_SELECT_LIMIT, OPT_MAX_JOIN_SIZE, OPT_SSL_SSL,
  OPT_SSL_KEY, OPT_SSL_CERT, OPT_SSL_CA, OPT_SSL_CAPATH,
  OPT_SSL_CIPHER, OPT_TLS_VERSION, OPT_SHUTDOWN_TIMEOUT, OPT_LOCAL_INFILE,
  OPT_DELETE_MASTER_LOGS, OPT_COMPACT,
  OPT_PROMPT, OPT_IGN_LINES,OPT_TRANSACTION,OPT_MYSQL_PROTOCOL,
  OPT_FRM, OPT_SKIP_OPTIMIZATION,
  OPT_COMPATIBLE, OPT_RECONNECT, OPT_DELIMITER, OPT_SECURE_AUTH,
  OPT_OPEN_FILES_LIMIT, OPT_SET_CHARSET, OPT_SERVER_ARG,
  OPT_STOP_POSITION, OPT_START_DATETIME, OPT_STOP_DATETIME,
  OPT_SIGINT_IGNORE, OPT_HEXBLOB, OPT_ORDER_BY_PRIMARY, OPT_COUNT,
  OPT_FLUSH_TABLES,
  OPT_TRIGGERS,
  OPT_MYSQL_ONLY_PRINT,
  OPT_MYSQL_LOCK_DIRECTORY,
  OPT_USE_THREADS,
  OPT_IMPORT_USE_THREADS,
  OPT_MYSQL_NUMBER_OF_QUERY,
  OPT_IGNORE_DATABASE,
  OPT_IGNORE_TABLE,OPT_INSERT_IGNORE,OPT_SHOW_WARNINGS,OPT_DROP_DATABASE,
  OPT_TZ_UTC, OPT_CREATE_SLAP_SCHEMA,
  OPT_MYSQLDUMP_SLAVE_APPLY,
  OPT_MYSQLDUMP_SLAVE_DATA,
  OPT_MYSQLDUMP_INCLUDE_MASTER_HOST_PORT,
#ifdef WHEN_FLASHBACK_REVIEW_READY
  OPT_REVIEW,
  OPT_REVIEW_DBNAME, OPT_REVIEW_TABLENAME,
#endif
  OPT_SLAP_CSV, OPT_SLAP_CREATE_STRING,
  OPT_SLAP_AUTO_GENERATE_SQL_LOAD_TYPE, OPT_SLAP_AUTO_GENERATE_WRITE_NUM,
  OPT_SLAP_AUTO_GENERATE_ADD_AUTO,
  OPT_SLAP_AUTO_GENERATE_GUID_PRIMARY,
  OPT_SLAP_AUTO_GENERATE_EXECUTE_QUERIES,
  OPT_SLAP_AUTO_GENERATE_SECONDARY_INDEXES,
  OPT_SLAP_AUTO_GENERATE_UNIQUE_WRITE_NUM,
  OPT_SLAP_AUTO_GENERATE_UNIQUE_QUERY_NUM,
  OPT_SLAP_PRE_QUERY,
  OPT_SLAP_POST_QUERY,
  OPT_SLAP_PRE_SYSTEM,
  OPT_SLAP_POST_SYSTEM,
  OPT_SLAP_COMMIT,
  OPT_SLAP_DETACH,
  OPT_SLAP_NO_DROP,
  OPT_MYSQL_REPLACE_INTO, OPT_BASE64_OUTPUT_MODE, OPT_SERVER_ID,
  OPT_FIX_TABLE_NAMES, OPT_FIX_DB_NAMES, OPT_SSL_VERIFY_SERVER_CERT,
  OPT_AUTO_VERTICAL_OUTPUT,
  OPT_DEBUG_INFO, OPT_DEBUG_CHECK, OPT_COLUMN_TYPES, OPT_ERROR_LOG_FILE,
  OPT_WRITE_BINLOG, OPT_DUMP_DATE,
  OPT_INIT_COMMAND,
  OPT_PLUGIN_DIR,
  OPT_DEFAULT_AUTH,
  OPT_ABORT_SOURCE_ON_ERROR,
  OPT_REWRITE_DB,
  OPT_REPORT_PROGRESS,
  OPT_SKIP_ANNOTATE_ROWS_EVENTS,
  OPT_SSL_CRL, OPT_SSL_CRLPATH,
  OPT_IGNORE_DATA,
  OPT_PRINT_ROW_COUNT, OPT_PRINT_ROW_EVENT_POSITIONS,
  OPT_CHECK_IF_UPGRADE_NEEDED,
  OPT_SHUTDOWN_WAIT_FOR_SLAVES,
  OPT_COPY_S3_TABLES,
  OPT_PRINT_TABLE_METADATA,
  OPT_ASOF_TIMESTAMP,
  OPT_IGNORE_DOMAIN_IDS,
  OPT_DO_DOMAIN_IDS,
  OPT_IGNORE_SERVER_IDS,
  OPT_DO_SERVER_IDS,
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

/**
  First mariadb version supporting the sys schema.
*/
#define FIRST_SYS_SCHEMA_VERSION 100600

/**
  Name of the sys schema database.
*/
#define SYS_SCHEMA_DB_NAME "sys"

/**
  The --socket CLI option has different meanings
  across different operating systems.
 */
#ifndef _WIN32
#define SOCKET_PROTOCOL_TO_FORCE MYSQL_PROTOCOL_SOCKET
#else
#define SOCKET_PROTOCOL_TO_FORCE MYSQL_PROTOCOL_PIPE
#endif

/**
  Utility function to implicitly change the connection protocol to a
  consistent value given the command line arguments.  Additionally,
  warns the user that the protocol has been changed.

  Arguments:
    @param [in] host              Name of the host to connect to
    @param [in, out] opt_protocol Location of the protocol option
                                  variable to update
    @param [in] new_protocol      New protocol to force
*/
static inline void warn_protocol_override(char *host,
                                          uint *opt_protocol,
                                          uint new_protocol)
{
  DBUG_ASSERT(new_protocol == MYSQL_PROTOCOL_TCP
      || new_protocol == SOCKET_PROTOCOL_TO_FORCE);


  if ((host == NULL
        || strncmp(host, LOCAL_HOST, sizeof(LOCAL_HOST)-1) == 0))
  {
    const char *protocol_name;

    if (*opt_protocol == MYSQL_PROTOCOL_DEFAULT
#ifndef _WIN32
        && new_protocol == MYSQL_PROTOCOL_SOCKET
#else
        && new_protocol == MYSQL_PROTOCOL_TCP
#endif
    )
    {
      /* This is already the default behavior, do nothing */
      return;
    }

    protocol_name= sql_protocol_typelib.type_names[new_protocol-1];

    fprintf(stderr, "%s %s %s\n",
        "WARNING: Forcing protocol to ",
        protocol_name,
        " due to option specification. "
          "Please explicitly state intended protocol.");

    *opt_protocol = new_protocol;
  }
}
