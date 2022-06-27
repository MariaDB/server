/* Copyright (c) 2000, 2011, Oracle and/or its affiliates.
   Copyright (c) 2010, 2017, MariaDB Corporation.

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

/*
** Common definition between mysql server & client
*/

#ifndef _mysql_com_h
#define _mysql_com_h

#include "my_decimal_limits.h"

#define HOSTNAME_LENGTH 255
#define HOSTNAME_LENGTH_STR STRINGIFY_ARG(HOSTNAME_LENGTH)
#define SYSTEM_CHARSET_MBMAXLEN 3
#define NAME_CHAR_LEN	64              /* Field/table name length */
#define USERNAME_CHAR_LENGTH 128
#define USERNAME_CHAR_LENGTH_STR STRINGIFY_ARG(USERNAME_CHAR_LENGTH)

#define NAME_LEN                (NAME_CHAR_LEN*SYSTEM_CHARSET_MBMAXLEN)
#define USERNAME_LENGTH         (USERNAME_CHAR_LENGTH*SYSTEM_CHARSET_MBMAXLEN)
#define DEFINER_CHAR_LENGTH     (USERNAME_CHAR_LENGTH + HOSTNAME_LENGTH + 1)
#define DEFINER_LENGTH          (USERNAME_LENGTH + HOSTNAME_LENGTH + 1)

#define MYSQL_AUTODETECT_CHARSET_NAME "auto"

#define MYSQL50_TABLE_NAME_PREFIX         "#mysql50#"
#define MYSQL50_TABLE_NAME_PREFIX_LENGTH  (sizeof(MYSQL50_TABLE_NAME_PREFIX)-1)
#define SAFE_NAME_LEN (NAME_LEN + MYSQL50_TABLE_NAME_PREFIX_LENGTH)

/*
  MDEV-4088

  MySQL (and MariaDB 5.x before the fix) was using the first character of the
  server version string (as sent in the first handshake protocol packet) to
  decide on the replication event formats. And for 10.x the first character
  is "1", which the slave thought comes from some ancient 1.x version
  (ignoring the fact that the first ever MySQL version was 3.x).

  To support replication to these old clients, we fake the version in the
  first handshake protocol packet to start from "5.5.5-" (for example,
  it might be "5.5.5-10.0.1-MariaDB-debug-log".

  On the client side we remove this fake version prefix to restore the
  correct server version. The version "5.5.5" did not support
  pluggable authentication, so any version starting from "5.5.5-" and
  claiming to support pluggable auth, must be using this fake prefix.
*/
/* this version must be the one that *does not* support pluggable auth */
#define RPL_VERSION_HACK "5.5.5-"

#define SERVER_VERSION_LENGTH 60
#define SQLSTATE_LENGTH 5
#define LIST_PROCESS_HOST_LEN 64

/*
  Maximum length of comments
*/
#define TABLE_COMMENT_INLINE_MAXLEN 180 /* pre 5.5: 60 characters */
#define TABLE_COMMENT_MAXLEN 2048
#define COLUMN_COMMENT_MAXLEN 1024
#define INDEX_COMMENT_MAXLEN 1024
#define TABLE_PARTITION_COMMENT_MAXLEN 1024
#define DATABASE_COMMENT_MAXLEN 1024

/*
  Maximum length of protocol packet.
  OK packet length limit also restricted to this value as any length greater
  than this value will have first byte of OK packet to be 254 thus does not
  provide a means to identify if this is OK or EOF packet.
*/
#define MAX_PACKET_LENGTH (256L*256L*256L-1)

/*
  USER_HOST_BUFF_SIZE -- length of string buffer, that is enough to contain
  username and hostname parts of the user identifier with trailing zero in
  MySQL standard format:
  user_name_part@host_name_part\0
*/
#define USER_HOST_BUFF_SIZE HOSTNAME_LENGTH + USERNAME_LENGTH + 2

#define LOCAL_HOST	"localhost"
#define LOCAL_HOST_NAMEDPIPE "."


#if defined(_WIN32) && !defined( _CUSTOMCONFIG_)
#define MYSQL_NAMEDPIPE "MySQL"
#define MYSQL_SERVICENAME "MySQL"
#endif

/*
  You should add new commands to the end of this list, otherwise old
  servers won't be able to handle them as 'unsupported'.
*/

enum enum_server_command
{
  COM_SLEEP, COM_QUIT, COM_INIT_DB, COM_QUERY, COM_FIELD_LIST,
  COM_CREATE_DB, COM_DROP_DB, COM_REFRESH, COM_SHUTDOWN, COM_STATISTICS,
  COM_PROCESS_INFO, COM_CONNECT, COM_PROCESS_KILL, COM_DEBUG, COM_PING,
  COM_TIME, COM_DELAYED_INSERT, COM_CHANGE_USER, COM_BINLOG_DUMP,
  COM_TABLE_DUMP, COM_CONNECT_OUT, COM_REGISTER_SLAVE,
  COM_STMT_PREPARE, COM_STMT_EXECUTE, COM_STMT_SEND_LONG_DATA, COM_STMT_CLOSE,
  COM_STMT_RESET, COM_SET_OPTION, COM_STMT_FETCH, COM_DAEMON,
  COM_UNIMPLEMENTED, /* COM_BINLOG_DUMP_GTID in MySQL */
  COM_RESET_CONNECTION,
  /* don't forget to update const char *command_name[] in sql_parse.cc */
  COM_MDB_GAP_BEG,
  COM_MDB_GAP_END=249,
  COM_STMT_BULK_EXECUTE=250,
  COM_SLAVE_WORKER=251,
  COM_SLAVE_IO=252,
  COM_SLAVE_SQL=253,
  COM_RESERVED_1=254, /* Old COM_MULTI, now removed */
  /* Must be last */
  COM_END=255
};


/*
  Bulk PS protocol indicator value:
*/
enum enum_indicator_type
{
  STMT_INDICATOR_NONE= 0,
  STMT_INDICATOR_NULL,
  STMT_INDICATOR_DEFAULT,
  STMT_INDICATOR_IGNORE
};

/*
  bulk PS flags
*/
#define STMT_BULK_FLAG_CLIENT_SEND_TYPES 128
#define STMT_BULK_FLAG_INSERT_ID_REQUEST 64


/* sql type stored in .frm files for virtual fields */
#define MYSQL_TYPE_VIRTUAL 245
/*
  Length of random string sent by server on handshake; this is also length of
  obfuscated password, received from client
*/
#define SCRAMBLE_LENGTH 20
#define SCRAMBLE_LENGTH_323 8
/* length of password stored in the db: new passwords are preceded with '*' */
#define SCRAMBLED_PASSWORD_CHAR_LENGTH (SCRAMBLE_LENGTH*2+1)
#define SCRAMBLED_PASSWORD_CHAR_LENGTH_323 (SCRAMBLE_LENGTH_323*2)


#define NOT_NULL_FLAG	1U		/* Field can't be NULL */
#define PRI_KEY_FLAG	2U		/* Field is part of a primary key */
#define UNIQUE_KEY_FLAG 4U		/* Field is part of a unique key */
#define MULTIPLE_KEY_FLAG 8U		/* Field is part of a key */
#define BLOB_FLAG	16U		/* Field is a blob */
#define UNSIGNED_FLAG	32U		/* Field is unsigned */
#define ZEROFILL_FLAG	64U		/* Field is zerofill */
#define BINARY_FLAG	128U		/* Field is binary   */

/* The following are only sent to new clients */
#define ENUM_FLAG	256U		/* field is an enum */
#define AUTO_INCREMENT_FLAG 512U	/* field is a autoincrement field */
#define TIMESTAMP_FLAG	1024U		/* Field is a timestamp */
#define SET_FLAG	2048U		/* field is a set */
#define NO_DEFAULT_VALUE_FLAG 4096U	/* Field doesn't have default value */
#define ON_UPDATE_NOW_FLAG 8192U	/* Field is set to NOW on UPDATE */
#define NUM_FLAG	32768U		/* Field is num (for clients) */
#define PART_KEY_FLAG	16384U		/* Intern; Part of some key */
#define GROUP_FLAG	32768U		/* Intern: Group field */
#define BINCMP_FLAG	131072U		/* Intern: Used by sql_yacc */
#define GET_FIXED_FIELDS_FLAG (1U << 18) /* Used to get fields in item tree */
#define FIELD_IN_PART_FUNC_FLAG (1U << 19)/* Field part of partition func */
#define PART_INDIRECT_KEY_FLAG (1U << 20)

/**
  Intern: Field in TABLE object for new version of altered table,
          which participates in a newly added index.
*/
#define FIELD_IN_ADD_INDEX (1U << 20)
#define FIELD_IS_RENAMED (1U << 21)     /* Intern: Field is being renamed */
#define FIELD_FLAGS_STORAGE_MEDIA 22    /* Field storage media, bit 22-23 */
#define FIELD_FLAGS_STORAGE_MEDIA_MASK (3U << FIELD_FLAGS_STORAGE_MEDIA)
#define FIELD_FLAGS_COLUMN_FORMAT 24    /* Field column format, bit 24-25 */
#define FIELD_FLAGS_COLUMN_FORMAT_MASK (3U << FIELD_FLAGS_COLUMN_FORMAT)
#define FIELD_IS_DROPPED (1U << 26)     /* Intern: Field is being dropped */

#define VERS_ROW_START (1 << 27)   /* autogenerated column declared with
                                             `generated always as row start`
                                              (see II.a SQL Standard) */
#define VERS_ROW_END (1 << 28)     /* autogenerated column declared with
                                           `generated always as row end`
                                            (see II.a SQL Standard).*/
#define VERS_SYSTEM_FIELD (VERS_ROW_START | VERS_ROW_END)
#define VERS_UPDATE_UNVERSIONED_FLAG (1 << 29) /* column that doesn't support
                                                system versioning when table
                                                itself supports it*/
#define LONG_UNIQUE_HASH_FIELD       (1<< 30) /* This field will store hash for unique
                                                column */
#define FIELD_PART_OF_TMP_UNIQUE     (1<< 31) /* part of an unique constrain
                                                for a tmporary table*/

#define REFRESH_GRANT           (1ULL << 0)  /* Refresh grant tables */
#define REFRESH_LOG             (1ULL << 1)  /* Start on new log file */
#define REFRESH_TABLES          (1ULL << 2)  /* close all tables */
#define REFRESH_HOSTS           (1ULL << 3)  /* Flush host cache */
#define REFRESH_STATUS          (1ULL << 4)  /* Flush status variables */
#define REFRESH_THREADS         (1ULL << 5)  /* Flush thread cache */
#define REFRESH_SLAVE           (1ULL << 6)  /* Reset master info and restart slave
                                             thread */
#define REFRESH_MASTER          (1ULL << 7)  /* Remove all bin logs in the index
                                             and truncate the index */

/* The following can't be set with mysql_refresh() */
#define REFRESH_ERROR_LOG       (1ULL << 8)  /* Rotate only the error log */
#define REFRESH_ENGINE_LOG      (1ULL << 9)  /* Flush all storage engine logs */
#define REFRESH_BINARY_LOG      (1ULL << 10) /* Flush the binary log */
#define REFRESH_RELAY_LOG       (1ULL << 11) /* Flush the relay log */
#define REFRESH_GENERAL_LOG     (1ULL << 12) /* Flush the general log */
#define REFRESH_SLOW_LOG        (1ULL << 13) /* Flush the slow query log */

#define REFRESH_READ_LOCK       (1ULL << 14) /* Lock tables for read */
#define REFRESH_CHECKPOINT      (1ULL << 15) /* With REFRESH_READ_LOCK: block checkpoints too */

#define REFRESH_QUERY_CACHE     (1ULL << 16) /* clear the query cache */
#define REFRESH_QUERY_CACHE_FREE (1ULL << 17) /* pack query cache */
#define REFRESH_DES_KEY_FILE    (1ULL << 18)
#define REFRESH_USER_RESOURCES  (1ULL << 19)
#define REFRESH_FOR_EXPORT      (1ULL << 20) /* FLUSH TABLES ... FOR EXPORT */
#define REFRESH_SSL             (1ULL << 21)

#define REFRESH_GENERIC         (1ULL << 30)
#define REFRESH_FAST            (1ULL << 31) /* Intern flag */

#define CLIENT_LONG_PASSWORD	0	/* obsolete flag */
#define CLIENT_MYSQL            1ULL       /* mysql/old mariadb server/client */
#define CLIENT_FOUND_ROWS	2ULL	/* Found instead of affected rows */
#define CLIENT_LONG_FLAG	4ULL	/* Get all column flags */
#define CLIENT_CONNECT_WITH_DB	8ULL	/* One can specify db on connect */
#define CLIENT_NO_SCHEMA	16ULL	/* Don't allow database.table.column */
#define CLIENT_COMPRESS		32ULL	/* Can use compression protocol */
#define CLIENT_ODBC		64ULL	/* Odbc client */
#define CLIENT_LOCAL_FILES	128ULL	/* Can use LOAD DATA LOCAL */
#define CLIENT_IGNORE_SPACE	256ULL	/* Ignore spaces before '(' */
#define CLIENT_PROTOCOL_41	512ULL	/* New 4.1 protocol */
#define CLIENT_INTERACTIVE	1024ULL	/* This is an interactive client */
#define CLIENT_SSL              2048ULL	/* Switch to SSL after handshake */
#define CLIENT_IGNORE_SIGPIPE   4096ULL    /* IGNORE sigpipes */
#define CLIENT_TRANSACTIONS	8192ULL	/* Client knows about transactions */
#define CLIENT_RESERVED         16384ULL   /* Old flag for 4.1 protocol  */
#define CLIENT_SECURE_CONNECTION 32768ULL  /* New 4.1 authentication */
#define CLIENT_MULTI_STATEMENTS (1ULL << 16) /* Enable/disable multi-stmt support */
#define CLIENT_MULTI_RESULTS    (1ULL << 17) /* Enable/disable multi-results */
#define CLIENT_PS_MULTI_RESULTS (1ULL << 18) /* Multi-results in PS-protocol */

#define CLIENT_PLUGIN_AUTH  (1ULL << 19) /* Client supports plugin authentication */
#define CLIENT_CONNECT_ATTRS (1ULL << 20) /* Client supports connection attributes */
/* Enable authentication response packet to be larger than 255 bytes. */
#define CLIENT_PLUGIN_AUTH_LENENC_CLIENT_DATA (1ULL << 21)
/* Don't close the connection for a connection with expired password. */
#define CLIENT_CAN_HANDLE_EXPIRED_PASSWORDS (1ULL << 22)

/**
  Capable of handling server state change information. Its a hint to the
  server to include the state change information in Ok packet.
*/
#define CLIENT_SESSION_TRACK (1ULL << 23)
/* Client no longer needs EOF packet */
#define CLIENT_DEPRECATE_EOF (1ULL << 24)

#define CLIENT_PROGRESS_OBSOLETE  (1ULL << 29)
#define CLIENT_SSL_VERIFY_SERVER_CERT (1ULL << 30)
/*
  It used to be that if mysql_real_connect() failed, it would delete any
  options set by the client, unless the CLIENT_REMEMBER_OPTIONS flag was
  given.
  That behaviour does not appear very useful, and it seems unlikely that
  any applications would actually depend on this. So from MariaDB 5.5 we
  always preserve any options set in case of failed connect, and this
  option is effectively always set.
*/
#define CLIENT_REMEMBER_OPTIONS (1ULL << 31)

/* MariaDB extended capability flags */
#define MARIADB_CLIENT_FLAGS_MASK 0xffffffff00000000ULL
/* Client support progress indicator */
#define MARIADB_CLIENT_PROGRESS (1ULL << 32)

/* Old COM_MULTI experiment (functionality removed).*/
#define MARIADB_CLIENT_RESERVED_1 (1ULL << 33)

/* support of array binding */
#define MARIADB_CLIENT_STMT_BULK_OPERATIONS (1ULL << 34)
/* support of extended metadata (e.g. type/format information) */
#define MARIADB_CLIENT_EXTENDED_METADATA (1ULL << 35)

/* Do not resend metadata for prepared statements, since 10.6*/
#define MARIADB_CLIENT_CACHE_METADATA (1ULL << 36)

#ifdef HAVE_COMPRESS
#define CAN_CLIENT_COMPRESS CLIENT_COMPRESS
#else
#define CAN_CLIENT_COMPRESS 0
#endif

/*
  Gather all possible capabilities (flags) supported by the server

  MARIADB_* flags supported only by MariaDB connector(s).
*/
#define CLIENT_ALL_FLAGS  (\
                           CLIENT_FOUND_ROWS | \
                           CLIENT_LONG_FLAG | \
                           CLIENT_CONNECT_WITH_DB | \
                           CLIENT_NO_SCHEMA | \
                           CLIENT_COMPRESS | \
                           CLIENT_ODBC | \
                           CLIENT_LOCAL_FILES | \
                           CLIENT_IGNORE_SPACE | \
                           CLIENT_PROTOCOL_41 | \
                           CLIENT_INTERACTIVE | \
                           CLIENT_SSL | \
                           CLIENT_IGNORE_SIGPIPE | \
                           CLIENT_TRANSACTIONS | \
                           CLIENT_RESERVED | \
                           CLIENT_SECURE_CONNECTION | \
                           CLIENT_MULTI_STATEMENTS | \
                           CLIENT_MULTI_RESULTS | \
                           CLIENT_PS_MULTI_RESULTS | \
                           CLIENT_SSL_VERIFY_SERVER_CERT | \
                           CLIENT_REMEMBER_OPTIONS | \
                           MARIADB_CLIENT_PROGRESS | \
                           CLIENT_PLUGIN_AUTH | \
                           CLIENT_PLUGIN_AUTH_LENENC_CLIENT_DATA | \
                           CLIENT_SESSION_TRACK |\
                           CLIENT_DEPRECATE_EOF |\
                           CLIENT_CONNECT_ATTRS |\
                           MARIADB_CLIENT_STMT_BULK_OPERATIONS |\
                           MARIADB_CLIENT_EXTENDED_METADATA|\
                           MARIADB_CLIENT_CACHE_METADATA |\
                           CLIENT_CAN_HANDLE_EXPIRED_PASSWORDS)
/*
  Switch off the flags that are optional and depending on build flags
  If any of the optional flags is supported by the build it will be switched
  on before sending to the client during the connection handshake.
*/
#define CLIENT_BASIC_FLAGS (((CLIENT_ALL_FLAGS & ~CLIENT_SSL) \
                                               & ~CLIENT_COMPRESS) \
                                               & ~CLIENT_SSL_VERIFY_SERVER_CERT)

enum mariadb_field_attr_t
{
  MARIADB_FIELD_ATTR_DATA_TYPE_NAME= 0,
  MARIADB_FIELD_ATTR_FORMAT_NAME= 1
};

#define MARIADB_FIELD_ATTR_LAST MARIADB_FIELD_ATTR_FORMAT_NAME


/**
  Is raised when a multi-statement transaction
  has been started, either explicitly, by means
  of BEGIN or COMMIT AND CHAIN, or
  implicitly, by the first transactional
  statement, when autocommit=off.
*/
#define SERVER_STATUS_IN_TRANS     1U
#define SERVER_STATUS_AUTOCOMMIT   2U	/* Server in auto_commit mode */
#define SERVER_MORE_RESULTS_EXISTS 8U   /* Multi query - next query exists */
#define SERVER_QUERY_NO_GOOD_INDEX_USED 16U
#define SERVER_QUERY_NO_INDEX_USED      32U
/**
  The server was able to fulfill the clients request and opened a
  read-only non-scrollable cursor for a query. This flag comes
  in reply to COM_STMT_EXECUTE and COM_STMT_FETCH commands.
*/
#define SERVER_STATUS_CURSOR_EXISTS 64U
/**
  This flag is sent when a read-only cursor is exhausted, in reply to
  COM_STMT_FETCH command.
*/
#define SERVER_STATUS_LAST_ROW_SENT 128U
#define SERVER_STATUS_DB_DROPPED        256U /* A database was dropped */
#define SERVER_STATUS_NO_BACKSLASH_ESCAPES 512U
/**
  Sent to the client if after a prepared statement reprepare
  we discovered that the new statement returns a different 
  number of result set columns.
*/
#define SERVER_STATUS_METADATA_CHANGED 1024U
#define SERVER_QUERY_WAS_SLOW          2048U

/**
  To mark ResultSet containing output parameter values.
*/
#define SERVER_PS_OUT_PARAMS            4096U

/**
  Set at the same time as SERVER_STATUS_IN_TRANS if the started
  multi-statement transaction is a read-only transaction. Cleared
  when the transaction commits or aborts. Since this flag is sent
  to clients in OK and EOF packets, the flag indicates the
  transaction status at the end of command execution.
*/
#define SERVER_STATUS_IN_TRANS_READONLY 8192U

/**
  This status flag, when on, implies that one of the state information has
  changed on the server because of the execution of the last statement.
*/
#define SERVER_SESSION_STATE_CHANGED    16384U

#define SERVER_STATUS_ANSI_QUOTES       32768U

/**
  Server status flags that must be cleared when starting
  execution of a new SQL statement.
  Flags from this set are only added to the
  current server status by the execution engine, but 
  never removed -- the execution engine expects them 
  to disappear automagically by the next command.
*/
#define SERVER_STATUS_CLEAR_SET (SERVER_QUERY_NO_GOOD_INDEX_USED| \
                                 SERVER_QUERY_NO_INDEX_USED|\
                                 SERVER_MORE_RESULTS_EXISTS|\
                                 SERVER_STATUS_METADATA_CHANGED |\
                                 SERVER_QUERY_WAS_SLOW |\
                                 SERVER_STATUS_DB_DROPPED |\
                                 SERVER_STATUS_CURSOR_EXISTS|\
                                 SERVER_STATUS_LAST_ROW_SENT|\
                                 SERVER_SESSION_STATE_CHANGED)

#define MYSQL_ERRMSG_SIZE	512
#define NET_READ_TIMEOUT	30		/* Timeout on read */
#define NET_WRITE_TIMEOUT	60		/* Timeout on write */
#define NET_WAIT_TIMEOUT	8*60*60		/* Wait for new query */

struct st_vio;					/* Only C */
typedef struct st_vio Vio;

#define MAX_TINYINT_WIDTH       3       /* Max width for a TINY w.o. sign */
#define MAX_SMALLINT_WIDTH      5       /* Max width for a SHORT w.o. sign */
#define MAX_MEDIUMINT_WIDTH     8       /* Max width for a INT24 w.o. sign */
#define MAX_INT_WIDTH           10      /* Max width for a LONG w.o. sign */
#define MAX_BIGINT_WIDTH        20      /* Max width for a LONGLONG */
#define MAX_CHAR_WIDTH		255	/* Max length for a CHAR column */
#define MAX_BLOB_WIDTH		16777216	/* Default width for blob */

typedef struct st_net {
#if !defined(CHECK_EMBEDDED_DIFFERENCES) || !defined(EMBEDDED_LIBRARY)
  Vio *vio;
  unsigned char *buff,*buff_end,*write_pos,*read_pos;
  my_socket fd;					/* For Perl DBI/dbd */
  /*
    The following variable is set if we are doing several queries in one
    command ( as in LOAD TABLE ... FROM MASTER ),
    and do not want to confuse the client with OK at the wrong time
  */
  unsigned long remain_in_buf,length, buf_length, where_b;
  unsigned long max_packet,max_packet_size;
  unsigned int pkt_nr,compress_pkt_nr;
  unsigned int write_timeout, read_timeout, retry_count;
  int fcntl;
  unsigned int *return_status;
  unsigned char reading_or_writing;
  char save_char;
  char net_skip_rest_factor;
  my_bool thread_specific_malloc;
  unsigned char compress;
  my_bool unused3; /* Please remove with the next incompatible ABI change. */
  /*
    Pointer to query object in query cache, do not equal NULL (0) for
    queries in cache that have not stored its results yet
  */
#endif
  void *thd; 	   /* Used by MariaDB server to avoid calling current_thd */
  unsigned int last_errno;
  unsigned char error; 
  my_bool unused4; /* Please remove with the next incompatible ABI change. */
  my_bool unused5; /* Please remove with the next incompatible ABI change. */
  /** Client library error message buffer. Actually belongs to struct MYSQL. */
  char last_error[MYSQL_ERRMSG_SIZE];
  /** Client library sqlstate buffer. Set along with the error message. */
  char sqlstate[SQLSTATE_LENGTH+1];
  void *extension;
} NET;


#define packet_error ~0UL

enum enum_field_types { MYSQL_TYPE_DECIMAL, MYSQL_TYPE_TINY,
			MYSQL_TYPE_SHORT,  MYSQL_TYPE_LONG,
			MYSQL_TYPE_FLOAT,  MYSQL_TYPE_DOUBLE,
			MYSQL_TYPE_NULL,   MYSQL_TYPE_TIMESTAMP,
			MYSQL_TYPE_LONGLONG,MYSQL_TYPE_INT24,
			MYSQL_TYPE_DATE,   MYSQL_TYPE_TIME,
			MYSQL_TYPE_DATETIME, MYSQL_TYPE_YEAR,
			MYSQL_TYPE_NEWDATE, MYSQL_TYPE_VARCHAR,
			MYSQL_TYPE_BIT,
                        /*
                          mysql-5.6 compatibility temporal types.
                          They're only used internally for reading RBR
                          mysql-5.6 binary log events and mysql-5.6 frm files.
                          They're never sent to the client.
                        */
                        MYSQL_TYPE_TIMESTAMP2,
                        MYSQL_TYPE_DATETIME2,
                        MYSQL_TYPE_TIME2,
                        /* Compressed types are only used internally for RBR. */
                        MYSQL_TYPE_BLOB_COMPRESSED= 140,
                        MYSQL_TYPE_VARCHAR_COMPRESSED= 141,

                        MYSQL_TYPE_NEWDECIMAL=246,
			MYSQL_TYPE_ENUM=247,
			MYSQL_TYPE_SET=248,
			MYSQL_TYPE_TINY_BLOB=249,
			MYSQL_TYPE_MEDIUM_BLOB=250,
			MYSQL_TYPE_LONG_BLOB=251,
			MYSQL_TYPE_BLOB=252,
			MYSQL_TYPE_VAR_STRING=253,
			MYSQL_TYPE_STRING=254,
			MYSQL_TYPE_GEOMETRY=255

};

/* For backward compatibility */
#define CLIENT_MULTI_QUERIES    CLIENT_MULTI_STATEMENTS    
#define FIELD_TYPE_DECIMAL     MYSQL_TYPE_DECIMAL
#define FIELD_TYPE_NEWDECIMAL  MYSQL_TYPE_NEWDECIMAL
#define FIELD_TYPE_TINY        MYSQL_TYPE_TINY
#define FIELD_TYPE_SHORT       MYSQL_TYPE_SHORT
#define FIELD_TYPE_LONG        MYSQL_TYPE_LONG
#define FIELD_TYPE_FLOAT       MYSQL_TYPE_FLOAT
#define FIELD_TYPE_DOUBLE      MYSQL_TYPE_DOUBLE
#define FIELD_TYPE_NULL        MYSQL_TYPE_NULL
#define FIELD_TYPE_TIMESTAMP   MYSQL_TYPE_TIMESTAMP
#define FIELD_TYPE_LONGLONG    MYSQL_TYPE_LONGLONG
#define FIELD_TYPE_INT24       MYSQL_TYPE_INT24
#define FIELD_TYPE_DATE        MYSQL_TYPE_DATE
#define FIELD_TYPE_TIME        MYSQL_TYPE_TIME
#define FIELD_TYPE_DATETIME    MYSQL_TYPE_DATETIME
#define FIELD_TYPE_YEAR        MYSQL_TYPE_YEAR
#define FIELD_TYPE_NEWDATE     MYSQL_TYPE_NEWDATE
#define FIELD_TYPE_ENUM        MYSQL_TYPE_ENUM
#define FIELD_TYPE_SET         MYSQL_TYPE_SET
#define FIELD_TYPE_TINY_BLOB   MYSQL_TYPE_TINY_BLOB
#define FIELD_TYPE_MEDIUM_BLOB MYSQL_TYPE_MEDIUM_BLOB
#define FIELD_TYPE_LONG_BLOB   MYSQL_TYPE_LONG_BLOB
#define FIELD_TYPE_BLOB        MYSQL_TYPE_BLOB
#define FIELD_TYPE_VAR_STRING  MYSQL_TYPE_VAR_STRING
#define FIELD_TYPE_STRING      MYSQL_TYPE_STRING
#define FIELD_TYPE_CHAR        MYSQL_TYPE_TINY
#define FIELD_TYPE_INTERVAL    MYSQL_TYPE_ENUM
#define FIELD_TYPE_GEOMETRY    MYSQL_TYPE_GEOMETRY
#define FIELD_TYPE_BIT         MYSQL_TYPE_BIT


/* Shutdown/kill enums and constants */ 

/* Bits for THD::killable. */
#define MYSQL_SHUTDOWN_KILLABLE_CONNECT    (unsigned char)(1 << 0)
#define MYSQL_SHUTDOWN_KILLABLE_TRANS      (unsigned char)(1 << 1)
#define MYSQL_SHUTDOWN_KILLABLE_LOCK_TABLE (unsigned char)(1 << 2)
#define MYSQL_SHUTDOWN_KILLABLE_UPDATE     (unsigned char)(1 << 3)

enum mysql_enum_shutdown_level {
  /*
    We want levels to be in growing order of hardness (because we use number
    comparisons). Note that DEFAULT does not respect the growing property, but
    it's ok.
  */
  SHUTDOWN_DEFAULT = 0,
  /* wait for existing connections to finish */
  SHUTDOWN_WAIT_CONNECTIONS= MYSQL_SHUTDOWN_KILLABLE_CONNECT,
  /* wait for existing trans to finish */
  SHUTDOWN_WAIT_TRANSACTIONS= MYSQL_SHUTDOWN_KILLABLE_TRANS,
  /* wait for existing updates to finish (=> no partial MyISAM update) */
  SHUTDOWN_WAIT_UPDATES= MYSQL_SHUTDOWN_KILLABLE_UPDATE,
  /* flush InnoDB buffers and other storage engines' buffers*/
  SHUTDOWN_WAIT_ALL_BUFFERS= (MYSQL_SHUTDOWN_KILLABLE_UPDATE << 1),
  /* don't flush InnoDB buffers, flush other storage engines' buffers*/
  SHUTDOWN_WAIT_CRITICAL_BUFFERS= (MYSQL_SHUTDOWN_KILLABLE_UPDATE << 1) + 1
};

enum enum_cursor_type
{
  CURSOR_TYPE_NO_CURSOR= 0,
  CURSOR_TYPE_READ_ONLY= 1,
  CURSOR_TYPE_FOR_UPDATE= 2,
  CURSOR_TYPE_SCROLLABLE= 4
};


/* options for mysql_set_option */
enum enum_mysql_set_option
{
  MYSQL_OPTION_MULTI_STATEMENTS_ON,
  MYSQL_OPTION_MULTI_STATEMENTS_OFF
};

/*
  Type of state change information that the server can include in the Ok
  packet.
*/
enum enum_session_state_type
{
  SESSION_TRACK_SYSTEM_VARIABLES,             /* Session system variables */
  SESSION_TRACK_SCHEMA,                       /* Current schema */
  SESSION_TRACK_STATE_CHANGE,                 /* track session state changes */
  SESSION_TRACK_GTIDS,
  SESSION_TRACK_TRANSACTION_CHARACTERISTICS,  /* Transaction chistics */
  SESSION_TRACK_TRANSACTION_STATE,            /* Transaction state */
#ifdef USER_VAR_TRACKING
  SESSION_TRACK_MYSQL_RESERVED1,
  SESSION_TRACK_MYSQL_RESERVED2,
  SESSION_TRACK_MYSQL_RESERVED3,
  SESSION_TRACK_MYSQL_RESERVED4,
  SESSION_TRACK_MYSQL_RESERVED5,
  SESSION_TRACK_MYSQL_RESERVED6,
  SESSION_TRACK_USER_VARIABLES,
#endif // USER_VAR_TRACKING
  SESSION_TRACK_always_at_the_end             /* must be last */
};

#define SESSION_TRACK_BEGIN SESSION_TRACK_SYSTEM_VARIABLES

#define IS_SESSION_STATE_TYPE(T) \
  (((int)(T) >= SESSION_TRACK_BEGIN) && ((T) < SESSION_TRACK_always_at_the_end))

#define net_new_transaction(net) ((net)->pkt_nr=0)

#ifdef __cplusplus
extern "C" {
#endif

my_bool	my_net_init(NET *net, Vio* vio, void *thd, unsigned int my_flags);
void	my_net_local_init(NET *net);
void	net_end(NET *net);
void	net_clear(NET *net, my_bool clear_buffer);
my_bool net_realloc(NET *net, size_t length);
my_bool	net_flush(NET *net);
my_bool	my_net_write(NET *net,const unsigned char *packet, size_t len);
my_bool	net_write_command(NET *net,unsigned char command,
			  const unsigned char *header, size_t head_len,
			  const unsigned char *packet, size_t len);
int	net_real_write(NET *net,const unsigned char *packet, size_t len);
unsigned long my_net_read_packet(NET *net, my_bool read_from_server);
unsigned long my_net_read_packet_reallen(NET *net, my_bool read_from_server,
                                         unsigned long* reallen);
#define my_net_read(A) my_net_read_packet((A), 0)

#ifdef MY_GLOBAL_INCLUDED
void my_net_set_write_timeout(NET *net, uint timeout);
void my_net_set_read_timeout(NET *net, uint timeout);
#endif

struct sockaddr;
int my_connect(my_socket s, const struct sockaddr *name, unsigned int namelen,
	       unsigned int timeout);
struct my_rnd_struct;

#ifdef __cplusplus
}
#endif

  /* The following is for user defined functions */

enum Item_result
{
  STRING_RESULT=0, REAL_RESULT, INT_RESULT, ROW_RESULT, DECIMAL_RESULT,
  TIME_RESULT
};

typedef struct st_udf_args
{
  unsigned int arg_count;		/* Number of arguments */
  enum Item_result *arg_type;		/* Pointer to item_results */
  char **args;				/* Pointer to argument */
  unsigned long *lengths;		/* Length of string arguments */
  char *maybe_null;			/* Set to 1 for all maybe_null args */
  const char **attributes;              /* Pointer to attribute name */
  unsigned long *attribute_lengths;     /* Length of attribute arguments */
  void *extension;
} UDF_ARGS;

  /* This holds information about the result */

typedef struct st_udf_init
{
  my_bool maybe_null;          /* 1 if function can return NULL */
  unsigned int decimals;       /* for real functions */
  unsigned long max_length;    /* For string functions */
  char *ptr;                   /* free pointer for function data */
  my_bool const_item;          /* 1 if function always returns the same value */
  void *extension;
} UDF_INIT;
/* 
  TODO: add a notion for determinism of the UDF. 
  See Item_udf_func::update_used_tables ()
*/

  /* Constants when using compression */
#define NET_HEADER_SIZE 4		/* standard header size */
#define COMP_HEADER_SIZE 3		/* compression header extra size */

  /* Prototypes to password functions */

#ifdef __cplusplus
extern "C" {
#endif

/*
  These functions are used for authentication by client and server and
  implemented in sql/password.c
*/

void create_random_string(char *to, unsigned int length,
                          struct my_rnd_struct *rand_st);

void hash_password(unsigned long *to, const char *password, unsigned int password_len);
void make_scrambled_password_323(char *to, const char *password);
void scramble_323(char *to, const char *message, const char *password);
my_bool check_scramble_323(const unsigned char *reply, const char *message,
                           unsigned long *salt);
void get_salt_from_password_323(unsigned long *res, const char *password);
void make_scrambled_password(char *to, const char *password);
void scramble(char *to, const char *message, const char *password);
my_bool check_scramble(const unsigned char *reply, const char *message,
                       const unsigned char *hash_stage2);
void get_salt_from_password(unsigned char *res, const char *password);
char *octet2hex(char *to, const char *str, size_t len);

/* end of password.c */

char *get_tty_password(const char *opt_message);
void get_tty_password_buff(const char *opt_message, char *to, size_t length);
const char *mysql_errno_to_sqlstate(unsigned int mysql_errno);

/* Some other useful functions */

my_bool my_thread_init(void);
void my_thread_end(void);

#ifdef MY_GLOBAL_INCLUDED
#include "pack.h"
#endif

#ifdef __cplusplus
}
#endif

#define NULL_LENGTH ~0UL /* For net_store_length */
#define MYSQL_STMT_HEADER       4U
#define MYSQL_LONG_DATA_HEADER  6U

/*
  If a float or double field have more than this number of decimals,
  it's regarded as floating point field without any specific number of
  decimals
*/


#endif
