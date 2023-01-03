#ifndef PRIVILEGE_H_INCLUDED
#define PRIVILEGE_H_INCLUDED

/* Copyright (c) 2020, MariaDB Corporation.

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

#include "my_global.h" // ulonglong


/*
  A strict enum to store privilege bits.

  We should eventually make if even stricter using "enum class privilege_t" and:
  - Replace all code pieces like `if (priv)` to `if (priv != NO_ACL)`
  - Remove "delete" comparison operators below
*/
enum privilege_t: unsigned long long
{
  NO_ACL                = (0),
  SELECT_ACL            = (1UL << 0),
  INSERT_ACL            = (1UL << 1),
  UPDATE_ACL            = (1UL << 2),
  DELETE_ACL            = (1UL << 3),
  CREATE_ACL            = (1UL << 4),
  DROP_ACL              = (1UL << 5),
  RELOAD_ACL            = (1UL << 6),
  SHUTDOWN_ACL          = (1UL << 7),
  PROCESS_ACL           = (1UL << 8),
  FILE_ACL              = (1UL << 9),
  GRANT_ACL             = (1UL << 10),
  REFERENCES_ACL        = (1UL << 11),
  INDEX_ACL             = (1UL << 12),
  ALTER_ACL             = (1UL << 13),
  SHOW_DB_ACL           = (1UL << 14),
  SUPER_ACL             = (1UL << 15),
  CREATE_TMP_ACL        = (1UL << 16),
  LOCK_TABLES_ACL       = (1UL << 17),
  EXECUTE_ACL           = (1UL << 18),
  REPL_SLAVE_ACL        = (1UL << 19),
  BINLOG_MONITOR_ACL    = (1UL << 20), // Was REPL_CLIENT_ACL prior to 10.5.2
  CREATE_VIEW_ACL       = (1UL << 21),
  SHOW_VIEW_ACL         = (1UL << 22),
  CREATE_PROC_ACL       = (1UL << 23),
  ALTER_PROC_ACL        = (1UL << 24),
  CREATE_USER_ACL       = (1UL << 25),
  EVENT_ACL             = (1UL << 26),
  TRIGGER_ACL           = (1UL << 27),
  CREATE_TABLESPACE_ACL = (1UL << 28),
  DELETE_HISTORY_ACL    = (1UL << 29),  // Added in 10.3.4
  SET_USER_ACL          = (1UL << 30),  // Added in 10.5.2
  FEDERATED_ADMIN_ACL   = (1UL << 31),  // Added in 10.5.2
  CONNECTION_ADMIN_ACL  = (1ULL << 32), // Added in 10.5.2
  READ_ONLY_ADMIN_ACL   = (1ULL << 33), // Added in 10.5.2
  REPL_SLAVE_ADMIN_ACL  = (1ULL << 34), // Added in 10.5.2
  REPL_MASTER_ADMIN_ACL = (1ULL << 35), // Added in 10.5.2
  BINLOG_ADMIN_ACL      = (1ULL << 36), // Added in 10.5.2
  BINLOG_REPLAY_ACL     = (1ULL << 37), // Added in 10.5.2
  SLAVE_MONITOR_ACL     = (1ULL << 38)  // Added in 10.5.8
  /*
    When adding new privilege bits, don't forget to update:
    In this file:
    - Add a new LAST_version_ACL
    - Add a new ALL_KNOWN_ACL_version
    - Change ALL_KNOWN_ACL to ALL_KNOWN_ACL_version
    - Change GLOBAL_ACLS if needed
    - Change SUPER_ADDED_SINCE_USER_TABLE_ACL if needed

    In other files:
    - static struct show_privileges_st sys_privileges[]
    - static const char *command_array[] and static uint command_lengths[]
    - mysql_system_tables.sql and mysql_system_tables_fix.sql
    - acl_init() or whatever - to define behaviour for old privilege tables
    - Update User_table_json::get_access()
    - sql_yacc.yy - for GRANT/REVOKE to work

    Important: the enum should contain only single-bit values.
    In this case, debuggers print bit combinations in the readable form:
     (gdb) p (privilege_t) (15)
     $8 = (SELECT_ACL | INSERT_ACL | UPDATE_ACL | DELETE_ACL)

    Bit-OR combinations of the above values should be declared outside!
  */
};

constexpr static inline privilege_t ALL_KNOWN_BITS(privilege_t x)
{
  return (privilege_t)(x | (x-1));
}

// Version markers
constexpr privilege_t LAST_100304_ACL= DELETE_HISTORY_ACL;
constexpr privilege_t LAST_100502_ACL= BINLOG_REPLAY_ACL;
constexpr privilege_t LAST_100508_ACL= SLAVE_MONITOR_ACL;

// Current version markers
constexpr privilege_t LAST_CURRENT_ACL= LAST_100508_ACL;
constexpr uint PRIVILEGE_T_MAX_BIT=
              my_bit_log2_uint64((ulonglong) LAST_CURRENT_ACL);

static_assert((privilege_t)(1ULL << PRIVILEGE_T_MAX_BIT) == LAST_CURRENT_ACL,
              "Something went fatally badly: "
              "LAST_CURRENT_ACL and PRIVILEGE_T_MAX_BIT do not match");

// A combination of all bits defined in 10.3.4 (and earlier)
constexpr privilege_t ALL_KNOWN_ACL_100304 = ALL_KNOWN_BITS(LAST_100304_ACL);

// A combination of all bits defined in 10.5.2
constexpr privilege_t ALL_KNOWN_ACL_100502= ALL_KNOWN_BITS(LAST_100502_ACL);

// A combination of all bits defined in 10.5.8
constexpr privilege_t ALL_KNOWN_ACL_100508= ALL_KNOWN_BITS(LAST_100508_ACL);
// unfortunately, SLAVE_MONITOR_ACL was added in 10.5.9, but also in 10.5.8-5
// let's stay compatible with that branch too.
constexpr privilege_t ALL_KNOWN_ACL_100509= ALL_KNOWN_ACL_100508;

// A combination of all bits defined as of the current version
constexpr privilege_t ALL_KNOWN_ACL= ALL_KNOWN_BITS(LAST_CURRENT_ACL);


// Unary operators
static inline constexpr ulonglong operator~(privilege_t access)
{
  return ~static_cast<ulonglong>(access);
}

/*
  Comparison operators.
  Delete automatic conversion between to/from integer types as much as possible.
  This forces to use `(priv == NO_ACL)` instead of `(priv == 0)`.

  Note: these operators will be gone when we change privilege_t to
  "enum class privilege_t". See comments above.
*/
static inline bool operator==(privilege_t, ulonglong)= delete;
static inline bool operator==(privilege_t,     ulong)= delete;
static inline bool operator==(privilege_t,      uint)= delete;
static inline bool operator==(privilege_t,     uchar)= delete;
static inline bool operator==(privilege_t,  longlong)= delete;
static inline bool operator==(privilege_t,      long)= delete;
static inline bool operator==(privilege_t,       int)= delete;
static inline bool operator==(privilege_t,      char)= delete;
static inline bool operator==(privilege_t,      bool)= delete;

static inline bool operator==(ulonglong, privilege_t)= delete;
static inline bool operator==(ulong,     privilege_t)= delete;
static inline bool operator==(uint,      privilege_t)= delete;
static inline bool operator==(uchar,     privilege_t)= delete;
static inline bool operator==(longlong,  privilege_t)= delete;
static inline bool operator==(long,      privilege_t)= delete;
static inline bool operator==(int,       privilege_t)= delete;
static inline bool operator==(char,      privilege_t)= delete;
static inline bool operator==(bool,      privilege_t)= delete;

static inline bool operator!=(privilege_t, ulonglong)= delete;
static inline bool operator!=(privilege_t,     ulong)= delete;
static inline bool operator!=(privilege_t,      uint)= delete;
static inline bool operator!=(privilege_t,     uchar)= delete;
static inline bool operator!=(privilege_t,  longlong)= delete;
static inline bool operator!=(privilege_t,      long)= delete;
static inline bool operator!=(privilege_t,       int)= delete;
static inline bool operator!=(privilege_t,      char)= delete;
static inline bool operator!=(privilege_t,      bool)= delete;

static inline bool operator!=(ulonglong, privilege_t)= delete;
static inline bool operator!=(ulong,     privilege_t)= delete;
static inline bool operator!=(uint,      privilege_t)= delete;
static inline bool operator!=(uchar,     privilege_t)= delete;
static inline bool operator!=(longlong,  privilege_t)= delete;
static inline bool operator!=(long,      privilege_t)= delete;
static inline bool operator!=(int,       privilege_t)= delete;
static inline bool operator!=(char,      privilege_t)= delete;
static inline bool operator!=(bool,      privilege_t)= delete;


// Dyadic bitwise operators
static inline constexpr privilege_t operator&(privilege_t a, privilege_t b)
{
  return static_cast<privilege_t>(static_cast<ulonglong>(a) &
                                  static_cast<ulonglong>(b));
}

static inline constexpr privilege_t operator&(ulonglong a, privilege_t b)
{
  return static_cast<privilege_t>(a & static_cast<ulonglong>(b));
}

static inline constexpr privilege_t operator&(privilege_t a, ulonglong b)
{
  return static_cast<privilege_t>(static_cast<ulonglong>(a) & b);
}

static inline constexpr privilege_t operator|(privilege_t a, privilege_t b)
{
  return static_cast<privilege_t>(static_cast<ulonglong>(a) |
                                  static_cast<ulonglong>(b));
}


// Dyadyc bitwise assignment operators
static inline privilege_t& operator&=(privilege_t &a, privilege_t b)
{
  return a= a & b;
}

static inline privilege_t& operator&=(privilege_t &a, ulonglong b)
{
  return a= a & b;
}

static inline privilege_t& operator|=(privilege_t &a, privilege_t b)
{
  return a= a | b;
}


/*
  A combination of all SUPER privileges added since the old user table format.
  These privileges are automatically added when upgrading from the
  old format mysql.user table if a user has the SUPER privilege.
*/
constexpr privilege_t  GLOBAL_SUPER_ADDED_SINCE_USER_TABLE_ACLS=
  SET_USER_ACL |
  FEDERATED_ADMIN_ACL |
  CONNECTION_ADMIN_ACL |
  READ_ONLY_ADMIN_ACL |
  REPL_SLAVE_ADMIN_ACL |
  BINLOG_ADMIN_ACL |
  BINLOG_REPLAY_ACL;


constexpr privilege_t COL_DML_ACLS=
  SELECT_ACL | INSERT_ACL | UPDATE_ACL | DELETE_ACL;

constexpr privilege_t VIEW_ACLS=
  CREATE_VIEW_ACL | SHOW_VIEW_ACL;

constexpr privilege_t STD_TABLE_DDL_ACLS=
  CREATE_ACL | DROP_ACL | ALTER_ACL;

constexpr privilege_t ALL_TABLE_DDL_ACLS=
  STD_TABLE_DDL_ACLS | INDEX_ACL;

constexpr privilege_t COL_ACLS=
  SELECT_ACL | INSERT_ACL | UPDATE_ACL | REFERENCES_ACL;

constexpr privilege_t PROC_DDL_ACLS=
  CREATE_PROC_ACL | ALTER_PROC_ACL;

constexpr privilege_t SHOW_PROC_ACLS=
  PROC_DDL_ACLS | EXECUTE_ACL;

constexpr privilege_t TABLE_ACLS=
  COL_DML_ACLS | ALL_TABLE_DDL_ACLS | VIEW_ACLS |
  GRANT_ACL | REFERENCES_ACL | 
  TRIGGER_ACL | DELETE_HISTORY_ACL;

constexpr privilege_t DB_ACLS=
   TABLE_ACLS | PROC_DDL_ACLS | EXECUTE_ACL |
   CREATE_TMP_ACL | LOCK_TABLES_ACL | EVENT_ACL;

constexpr privilege_t PROC_ACLS=
  ALTER_PROC_ACL | EXECUTE_ACL | GRANT_ACL;

constexpr privilege_t GLOBAL_ACLS=
  DB_ACLS | SHOW_DB_ACL |
  CREATE_USER_ACL | CREATE_TABLESPACE_ACL |
  SUPER_ACL | RELOAD_ACL | SHUTDOWN_ACL | PROCESS_ACL | FILE_ACL |
  REPL_SLAVE_ACL | BINLOG_MONITOR_ACL |
  GLOBAL_SUPER_ADDED_SINCE_USER_TABLE_ACLS |
  REPL_MASTER_ADMIN_ACL | SLAVE_MONITOR_ACL;

constexpr privilege_t DEFAULT_CREATE_PROC_ACLS=
  ALTER_PROC_ACL | EXECUTE_ACL;

constexpr privilege_t SHOW_CREATE_TABLE_ACLS=
  COL_DML_ACLS | ALL_TABLE_DDL_ACLS |
  TRIGGER_ACL | REFERENCES_ACL | GRANT_ACL | VIEW_ACLS;

/**
  Table-level privileges which are automatically "granted" to everyone on
  existing temporary tables (CREATE_ACL is necessary for ALTER ... RENAME).
*/
constexpr privilege_t TMP_TABLE_ACLS=
  COL_DML_ACLS | ALL_TABLE_DDL_ACLS | REFERENCES_ACL;


constexpr privilege_t PRIV_LOCK_TABLES= SELECT_ACL | LOCK_TABLES_ACL;

/*
  Allow to set an object definer:
    CREATE DEFINER=xxx {TRIGGER|VIEW|FUNCTION|PROCEDURE}
  Was SUPER prior to 10.5.2
*/
constexpr privilege_t PRIV_DEFINER_CLAUSE= SET_USER_ACL | SUPER_ACL;
/*
  If a VIEW has a `definer=invoker@host` clause and
  the specified definer does not exists, then
  - The invoker with REVEAL_MISSING_DEFINER_ACL gets:
    ERROR: The user specified as a definer ('definer1'@'localhost') doesn't exist
  - The invoker without MISSING_DEFINER_ACL gets a generic access error,
    without revealing details that the definer does not exists.

  TODO: we should eventually test the same privilege when processing
  other objects that have the DEFINER clause (e.g. routines, triggers).
  Currently the missing definer is revealed for non-privileged invokers
  in case of routines, triggers, etc.

  Was SUPER prior to 10.5.2
*/
constexpr privilege_t PRIV_REVEAL_MISSING_DEFINER= SET_USER_ACL | SUPER_ACL;

/* Actions that require only the SUPER privilege */
constexpr privilege_t PRIV_DES_DECRYPT_ONE_ARG= SUPER_ACL;
constexpr privilege_t PRIV_LOG_BIN_TRUSTED_SP_CREATOR= SUPER_ACL;
constexpr privilege_t PRIV_DEBUG= SUPER_ACL;
constexpr privilege_t PRIV_SET_GLOBAL_SYSTEM_VARIABLE= SUPER_ACL;
constexpr privilege_t PRIV_SET_RESTRICTED_SESSION_SYSTEM_VARIABLE= SUPER_ACL;

/* The following variables respected only SUPER_ACL prior to 10.5.2 */
constexpr privilege_t PRIV_SET_SYSTEM_VAR_BINLOG_FORMAT=
  SUPER_ACL | BINLOG_ADMIN_ACL;

constexpr privilege_t PRIV_SET_SYSTEM_VAR_BINLOG_DIRECT_NON_TRANSACTIONAL_UPDATES=
  SUPER_ACL | BINLOG_ADMIN_ACL;

constexpr privilege_t PRIV_SET_SYSTEM_VAR_BINLOG_ANNOTATE_ROW_EVENTS=
  SUPER_ACL | BINLOG_ADMIN_ACL;

constexpr privilege_t PRIV_SET_SYSTEM_VAR_BINLOG_ROW_IMAGE=
  SUPER_ACL | BINLOG_ADMIN_ACL;

constexpr privilege_t PRIV_SET_SYSTEM_VAR_SQL_LOG_BIN=
  SUPER_ACL | BINLOG_ADMIN_ACL;

constexpr privilege_t PRIV_SET_SYSTEM_GLOBAL_VAR_BINLOG_CACHE_SIZE=
  SUPER_ACL | BINLOG_ADMIN_ACL;

constexpr privilege_t PRIV_SET_SYSTEM_GLOBAL_VAR_BINLOG_FILE_CACHE_SIZE=
  SUPER_ACL | BINLOG_ADMIN_ACL;

constexpr privilege_t PRIV_SET_SYSTEM_GLOBAL_VAR_BINLOG_STMT_CACHE_SIZE=
  SUPER_ACL | BINLOG_ADMIN_ACL;

constexpr privilege_t PRIV_SET_SYSTEM_GLOBAL_VAR_BINLOG_COMMIT_WAIT_COUNT=
  SUPER_ACL | BINLOG_ADMIN_ACL;

constexpr privilege_t PRIV_SET_SYSTEM_GLOBAL_VAR_BINLOG_COMMIT_WAIT_USEC=
  SUPER_ACL | BINLOG_ADMIN_ACL;

constexpr privilege_t PRIV_SET_SYSTEM_GLOBAL_VAR_BINLOG_ROW_METADATA=
  SUPER_ACL | BINLOG_ADMIN_ACL;

constexpr privilege_t PRIV_SET_SYSTEM_GLOBAL_VAR_EXPIRE_LOGS_DAYS=
  SUPER_ACL | BINLOG_ADMIN_ACL;

constexpr privilege_t PRIV_SET_SYSTEM_GLOBAL_VAR_LOG_BIN_COMPRESS=
  SUPER_ACL | BINLOG_ADMIN_ACL;

constexpr privilege_t PRIV_SET_SYSTEM_GLOBAL_VAR_LOG_BIN_COMPRESS_MIN_LEN=
  SUPER_ACL | BINLOG_ADMIN_ACL;

constexpr privilege_t PRIV_SET_SYSTEM_GLOBAL_VAR_LOG_BIN_TRUST_FUNCTION_CREATORS=
  SUPER_ACL | BINLOG_ADMIN_ACL;

constexpr privilege_t PRIV_SET_SYSTEM_GLOBAL_VAR_MAX_BINLOG_CACHE_SIZE=
  SUPER_ACL | BINLOG_ADMIN_ACL;

constexpr privilege_t PRIV_SET_SYSTEM_GLOBAL_VAR_MAX_BINLOG_STMT_CACHE_SIZE=
  SUPER_ACL | BINLOG_ADMIN_ACL;

constexpr privilege_t PRIV_SET_SYSTEM_GLOBAL_VAR_MAX_BINLOG_SIZE=
  SUPER_ACL | BINLOG_ADMIN_ACL;

constexpr privilege_t PRIV_SET_SYSTEM_GLOBAL_VAR_SYNC_BINLOG=
  SUPER_ACL | BINLOG_ADMIN_ACL;



/* Privileges related to --read-only */
// Was super prior to 10.5.2
constexpr privilege_t PRIV_IGNORE_READ_ONLY= READ_ONLY_ADMIN_ACL;
// Was super prior to 10.5.2
constexpr privilege_t PRIV_SET_SYSTEM_GLOBAL_VAR_READ_ONLY=
  READ_ONLY_ADMIN_ACL;

/*
  Privileges related to connection handling.
*/
// Was SUPER_ACL prior to 10.5.2
constexpr privilege_t PRIV_IGNORE_INIT_CONNECT= CONNECTION_ADMIN_ACL | SUPER_ACL;
// Was SUPER_ACL prior to 10.5.2
constexpr privilege_t PRIV_IGNORE_MAX_USER_CONNECTIONS= CONNECTION_ADMIN_ACL | SUPER_ACL;
// Was SUPER_ACL prior to 10.5.2
constexpr privilege_t PRIV_IGNORE_MAX_CONNECTIONS= CONNECTION_ADMIN_ACL | SUPER_ACL;
// Was SUPER_ACL prior to 10.5.2
constexpr privilege_t PRIV_IGNORE_MAX_PASSWORD_ERRORS= CONNECTION_ADMIN_ACL | SUPER_ACL;
// Was SUPER_ACL prior to 10.5.2
constexpr privilege_t PRIV_KILL_OTHER_USER_PROCESS= CONNECTION_ADMIN_ACL | SUPER_ACL;

// Was SUPER_ACL prior to 10.5.2
constexpr privilege_t PRIV_SET_SYSTEM_GLOBAL_VAR_CONNECT_TIMEOUT=
  CONNECTION_ADMIN_ACL | SUPER_ACL;
// Was SUPER_ACL prior to 10.5.2
constexpr privilege_t PRIV_SET_SYSTEM_GLOBAL_VAR_DISCONNECT_ON_EXPIRED_PASSWORD=
  CONNECTION_ADMIN_ACL | SUPER_ACL;
// Was SUPER_ACL prior to 10.5.2
constexpr privilege_t PRIV_SET_SYSTEM_GLOBAL_VAR_EXTRA_MAX_CONNECTIONS=
  CONNECTION_ADMIN_ACL | SUPER_ACL;
// Was SUPER_ACL prior to 10.5.2
constexpr privilege_t PRIV_SET_SYSTEM_GLOBAL_VAR_INIT_CONNECT=
  CONNECTION_ADMIN_ACL | SUPER_ACL;
// Was SUPER_ACL prior to 10.5.2
constexpr privilege_t PRIV_SET_SYSTEM_GLOBAL_VAR_MAX_CONNECTIONS=
  CONNECTION_ADMIN_ACL | SUPER_ACL;
// Was SUPER_ACL prior to 10.5.2
constexpr privilege_t PRIV_SET_SYSTEM_GLOBAL_VAR_MAX_CONNECT_ERRORS=
  CONNECTION_ADMIN_ACL | SUPER_ACL;
// Was SUPER_ACL prior to 10.5.2
constexpr privilege_t PRIV_SET_SYSTEM_GLOBAL_VAR_MAX_PASSWORD_ERRORS=
  CONNECTION_ADMIN_ACL | SUPER_ACL;
// Was SUPER_ACL prior to 10.5.2
constexpr privilege_t PRIV_SET_SYSTEM_GLOBAL_VAR_PROXY_PROTOCOL_NETWORKS=
  CONNECTION_ADMIN_ACL | SUPER_ACL;
// Was SUPER_ACL prior to 10.5.2
constexpr privilege_t PRIV_SET_SYSTEM_GLOBAL_VAR_SECURE_AUTH=
  CONNECTION_ADMIN_ACL | SUPER_ACL;
// Was SUPER_ACL prior to 10.5.2
constexpr privilege_t PRIV_SET_SYSTEM_GLOBAL_VAR_SLOW_LAUNCH_TIME=
  CONNECTION_ADMIN_ACL | SUPER_ACL;

// Was SUPER_ACL prior to 10.5.2
constexpr privilege_t PRIV_SET_SYSTEM_GLOBAL_VAR_THREAD_POOL=
  CONNECTION_ADMIN_ACL | SUPER_ACL;


/*
  Binary log related privileges that are checked regardless
  of active replication running.
*/

/*
  This command was renamed from "SHOW MASTER STATUS"
  to "SHOW BINLOG STATUS" in 10.5.2.
  Was SUPER_ACL | REPL_CLIENT_ACL prior to 10.5.2
  REPL_CLIENT_ACL was renamed to BINLOG_MONITOR_ACL.
*/
constexpr privilege_t PRIV_STMT_SHOW_BINLOG_STATUS= BINLOG_MONITOR_ACL | SUPER_ACL;

/*
  Was SUPER_ACL | REPL_CLIENT_ACL prior to 10.5.2
  REPL_CLIENT_ACL was renamed to BINLOG_MONITOR_ACL.
*/
constexpr privilege_t PRIV_STMT_SHOW_BINARY_LOGS= BINLOG_MONITOR_ACL | SUPER_ACL;

// Was SUPER_ACL prior to 10.5.2
constexpr privilege_t PRIV_STMT_PURGE_BINLOG= BINLOG_ADMIN_ACL | SUPER_ACL;

// Was REPL_SLAVE_ACL prior to 10.5.2
constexpr privilege_t PRIV_STMT_SHOW_BINLOG_EVENTS= BINLOG_MONITOR_ACL;


/*
  Privileges for replication related statements and commands
  that are executed on the master.
*/
constexpr privilege_t PRIV_COM_REGISTER_SLAVE= REPL_SLAVE_ACL;
constexpr privilege_t PRIV_COM_BINLOG_DUMP= REPL_SLAVE_ACL;
// Was REPL_SLAVE_ACL prior to 10.5.2
constexpr privilege_t PRIV_STMT_SHOW_SLAVE_HOSTS= REPL_MASTER_ADMIN_ACL;

/*
  Replication master related variable privileges.
  Where SUPER prior to 10.5.2
*/
constexpr privilege_t PRIV_SET_SYSTEM_GLOBAL_VAR_RPL_SEMI_SYNC_MASTER_ENABLED=
  REPL_MASTER_ADMIN_ACL | SUPER_ACL;
constexpr privilege_t PRIV_SET_SYSTEM_GLOBAL_VAR_RPL_SEMI_SYNC_MASTER_TIMEOUT=
  REPL_MASTER_ADMIN_ACL | SUPER_ACL;
constexpr privilege_t PRIV_SET_SYSTEM_GLOBAL_VAR_RPL_SEMI_SYNC_MASTER_WAIT_NO_SLAVE=
  REPL_MASTER_ADMIN_ACL | SUPER_ACL;
constexpr privilege_t PRIV_SET_SYSTEM_GLOBAL_VAR_RPL_SEMI_SYNC_MASTER_TRACE_LEVEL=
  REPL_MASTER_ADMIN_ACL | SUPER_ACL;
constexpr privilege_t PRIV_SET_SYSTEM_GLOBAL_VAR_RPL_SEMI_SYNC_MASTER_WAIT_POINT=
  REPL_MASTER_ADMIN_ACL | SUPER_ACL;

constexpr privilege_t PRIV_SET_SYSTEM_GLOBAL_VAR_MASTER_VERIFY_CHECKSUM=
  REPL_MASTER_ADMIN_ACL | SUPER_ACL;

constexpr privilege_t PRIV_SET_SYSTEM_GLOBAL_VAR_GTID_BINLOG_STATE=
  REPL_MASTER_ADMIN_ACL | SUPER_ACL;

constexpr privilege_t PRIV_SET_SYSTEM_GLOBAL_VAR_SERVER_ID=
  REPL_MASTER_ADMIN_ACL | SUPER_ACL;

constexpr privilege_t PRIV_SET_SYSTEM_GLOBAL_VAR_GTID_DOMAIN_ID=
  REPL_MASTER_ADMIN_ACL | SUPER_ACL;


/* Privileges for statements that are executed on the slave */
// Was SUPER_ACL prior to 10.5.2
constexpr privilege_t PRIV_STMT_START_SLAVE= REPL_SLAVE_ADMIN_ACL | SUPER_ACL;
// Was SUPER_ACL prior to 10.5.2
constexpr privilege_t PRIV_STMT_STOP_SLAVE= REPL_SLAVE_ADMIN_ACL | SUPER_ACL;
// Was SUPER_ACL prior to 10.5.2
constexpr privilege_t PRIV_STMT_CHANGE_MASTER= REPL_SLAVE_ADMIN_ACL | SUPER_ACL;
// Was (SUPER_ACL | REPL_CLIENT_ACL) prior to 10.5.2
// Was (SUPER_ACL | REPL_SLAVE_ADMIN_ACL) from 10.5.2 to 10.5.7
constexpr privilege_t PRIV_STMT_SHOW_SLAVE_STATUS= SLAVE_MONITOR_ACL | SUPER_ACL;
// Was REPL_SLAVE_ACL prior to 10.5.2
// Was REPL_SLAVE_ADMIN_ACL from 10.5.2 to 10.5.7
constexpr privilege_t PRIV_STMT_SHOW_RELAYLOG_EVENTS= SLAVE_MONITOR_ACL;

/*
  Privileges related to binlog replying.
  Were SUPER_ACL prior to 10.5.2
*/
constexpr privilege_t PRIV_STMT_BINLOG= BINLOG_REPLAY_ACL | SUPER_ACL;

constexpr privilege_t PRIV_SET_SYSTEM_SESSION_VAR_GTID_SEQ_NO=
  BINLOG_REPLAY_ACL | SUPER_ACL;

constexpr privilege_t PRIV_SET_SYSTEM_SESSION_VAR_PSEUDO_THREAD_ID=
  BINLOG_REPLAY_ACL | SUPER_ACL;

constexpr privilege_t PRIV_SET_SYSTEM_SESSION_VAR_SERVER_ID=
  BINLOG_REPLAY_ACL | SUPER_ACL;

constexpr privilege_t PRIV_SET_SYSTEM_SESSION_VAR_GTID_DOMAIN_ID=
  BINLOG_REPLAY_ACL | SUPER_ACL;

/*
  Privileges for slave related global variables.
  Were SUPER prior to 10.5.2.
*/
constexpr privilege_t PRIV_SET_SYSTEM_GLOBAL_VAR_REPLICATE_EVENTS_MARKED_FOR_SKIP=
  REPL_SLAVE_ADMIN_ACL | SUPER_ACL;
constexpr privilege_t PRIV_SET_SYSTEM_GLOBAL_VAR_REPLICATE_REWRITE_DB=
  REPL_SLAVE_ADMIN_ACL | SUPER_ACL;
constexpr privilege_t PRIV_SET_SYSTEM_GLOBAL_VAR_REPLICATE_DO_DB=
  REPL_SLAVE_ADMIN_ACL | SUPER_ACL;
constexpr privilege_t PRIV_SET_SYSTEM_GLOBAL_VAR_REPLICATE_DO_TABLE=
  REPL_SLAVE_ADMIN_ACL | SUPER_ACL;
constexpr privilege_t PRIV_SET_SYSTEM_GLOBAL_VAR_REPLICATE_IGNORE_DB=
  REPL_SLAVE_ADMIN_ACL | SUPER_ACL;
constexpr privilege_t PRIV_SET_SYSTEM_GLOBAL_VAR_REPLICATE_IGNORE_TABLE=
  REPL_SLAVE_ADMIN_ACL | SUPER_ACL;
constexpr privilege_t PRIV_SET_SYSTEM_GLOBAL_VAR_REPLICATE_WILD_DO_TABLE=
  REPL_SLAVE_ADMIN_ACL | SUPER_ACL;
constexpr privilege_t PRIV_SET_SYSTEM_GLOBAL_VAR_REPLICATE_WILD_IGNORE_TABLE=
  REPL_SLAVE_ADMIN_ACL | SUPER_ACL;
constexpr privilege_t PRIV_SET_SYSTEM_GLOBAL_VAR_READ_BINLOG_SPEED_LIMIT=
  REPL_SLAVE_ADMIN_ACL | SUPER_ACL;
constexpr privilege_t PRIV_SET_SYSTEM_GLOBAL_VAR_SLAVE_COMPRESSED_PROTOCOL=
  REPL_SLAVE_ADMIN_ACL | SUPER_ACL;
constexpr privilege_t PRIV_SET_SYSTEM_GLOBAL_VAR_SLAVE_DDL_EXEC_MODE=
  REPL_SLAVE_ADMIN_ACL | SUPER_ACL;
constexpr privilege_t PRIV_SET_SYSTEM_GLOBAL_VAR_SLAVE_DOMAIN_PARALLEL_THREADS=
  REPL_SLAVE_ADMIN_ACL | SUPER_ACL;
constexpr privilege_t PRIV_SET_SYSTEM_GLOBAL_VAR_SLAVE_EXEC_MODE=
  REPL_SLAVE_ADMIN_ACL | SUPER_ACL;
constexpr privilege_t PRIV_SET_SYSTEM_GLOBAL_VAR_SLAVE_MAX_ALLOWED_PACKET=
  REPL_SLAVE_ADMIN_ACL | SUPER_ACL;
constexpr privilege_t PRIV_SET_SYSTEM_GLOBAL_VAR_SLAVE_MAX_STATEMENT_TIME=
  REPL_SLAVE_ADMIN_ACL;
constexpr privilege_t PRIV_SET_SYSTEM_GLOBAL_VAR_SLAVE_NET_TIMEOUT=
  REPL_SLAVE_ADMIN_ACL | SUPER_ACL;
constexpr privilege_t PRIV_SET_SYSTEM_GLOBAL_VAR_SLAVE_PARALLEL_MAX_QUEUED=
  REPL_SLAVE_ADMIN_ACL | SUPER_ACL;
constexpr privilege_t PRIV_SET_SYSTEM_GLOBAL_VAR_SLAVE_PARALLEL_MODE=
  REPL_SLAVE_ADMIN_ACL | SUPER_ACL;
constexpr privilege_t PRIV_SET_SYSTEM_GLOBAL_VAR_SLAVE_PARALLEL_THREADS=
  REPL_SLAVE_ADMIN_ACL | SUPER_ACL;
constexpr privilege_t PRIV_SET_SYSTEM_GLOBAL_VAR_SLAVE_PARALLEL_WORKERS=
  REPL_SLAVE_ADMIN_ACL | SUPER_ACL;
constexpr privilege_t PRIV_SET_SYSTEM_GLOBAL_VAR_SLAVE_RUN_TRIGGERS_FOR_RBR=
  REPL_SLAVE_ADMIN_ACL | SUPER_ACL;
constexpr privilege_t PRIV_SET_SYSTEM_GLOBAL_VAR_SLAVE_SQL_VERIFY_CHECKSUM=
  REPL_SLAVE_ADMIN_ACL | SUPER_ACL;
constexpr privilege_t PRIV_SET_SYSTEM_GLOBAL_VAR_SLAVE_TRANSACTION_RETRY_INTERVAL=
  REPL_SLAVE_ADMIN_ACL | SUPER_ACL;
constexpr privilege_t PRIV_SET_SYSTEM_GLOBAL_VAR_SLAVE_TYPE_CONVERSIONS=
  REPL_SLAVE_ADMIN_ACL | SUPER_ACL;
constexpr privilege_t PRIV_SET_SYSTEM_GLOBAL_VAR_INIT_SLAVE=
  REPL_SLAVE_ADMIN_ACL | SUPER_ACL;

constexpr privilege_t PRIV_SET_SYSTEM_GLOBAL_VAR_RPL_SEMI_SYNC_SLAVE_ENABLED=
  REPL_SLAVE_ADMIN_ACL | SUPER_ACL;
constexpr privilege_t PRIV_SET_SYSTEM_GLOBAL_VAR_RPL_SEMI_SYNC_SLAVE_TRACE_LEVEL=
  REPL_SLAVE_ADMIN_ACL | SUPER_ACL;
constexpr privilege_t PRIV_SET_SYSTEM_GLOBAL_VAR_RPL_SEMI_SYNC_SLAVE_DELAY_MASTER=
  REPL_SLAVE_ADMIN_ACL | SUPER_ACL;
constexpr privilege_t PRIV_SET_SYSTEM_GLOBAL_VAR_RPL_SEMI_SYNC_SLAVE_KILL_CONN_TIMEOUT=
  REPL_SLAVE_ADMIN_ACL | SUPER_ACL;

constexpr privilege_t PRIV_SET_SYSTEM_GLOBAL_VAR_RELAY_LOG_PURGE=
  REPL_SLAVE_ADMIN_ACL | SUPER_ACL;
constexpr privilege_t PRIV_SET_SYSTEM_GLOBAL_VAR_RELAY_LOG_RECOVERY=
  REPL_SLAVE_ADMIN_ACL | SUPER_ACL;
constexpr privilege_t PRIV_SET_SYSTEM_GLOBAL_VAR_SYNC_MASTER_INFO=
  REPL_SLAVE_ADMIN_ACL | SUPER_ACL;
constexpr privilege_t PRIV_SET_SYSTEM_GLOBAL_VAR_SYNC_RELAY_LOG=
  REPL_SLAVE_ADMIN_ACL | SUPER_ACL;
constexpr privilege_t PRIV_SET_SYSTEM_GLOBAL_VAR_SYNC_RELAY_LOG_INFO=
  REPL_SLAVE_ADMIN_ACL | SUPER_ACL;

constexpr privilege_t PRIV_SET_SYSTEM_GLOBAL_VAR_GTID_CLEANUP_BATCH_SIZE=
  REPL_SLAVE_ADMIN_ACL | SUPER_ACL;
constexpr privilege_t PRIV_SET_SYSTEM_GLOBAL_VAR_GTID_IGNORE_DUPLICATES=
  REPL_SLAVE_ADMIN_ACL | SUPER_ACL;
constexpr privilege_t PRIV_SET_SYSTEM_GLOBAL_VAR_GTID_POS_AUTO_ENGINES=
  REPL_SLAVE_ADMIN_ACL | SUPER_ACL;
constexpr privilege_t PRIV_SET_SYSTEM_GLOBAL_VAR_GTID_SLAVE_POS=
  REPL_SLAVE_ADMIN_ACL | SUPER_ACL;
constexpr privilege_t PRIV_SET_SYSTEM_GLOBAL_VAR_GTID_STRICT_MODE=
  REPL_SLAVE_ADMIN_ACL | SUPER_ACL;


/* Privileges for federated database related statements */
// Was SUPER_ACL prior to 10.5.2
constexpr privilege_t PRIV_STMT_CREATE_SERVER= FEDERATED_ADMIN_ACL | SUPER_ACL;
// Was SUPER_ACL prior to 10.5.2
constexpr privilege_t PRIV_STMT_ALTER_SERVER= FEDERATED_ADMIN_ACL | SUPER_ACL;
// Was SUPER_ACL prior to 10.5.2
constexpr privilege_t PRIV_STMT_DROP_SERVER= FEDERATED_ADMIN_ACL | SUPER_ACL;


/* Privileges related to processes */
constexpr privilege_t PRIV_COM_PROCESS_INFO= PROCESS_ACL;
// This privilege applies both for SHOW EXPLAIN and SHOW ANALYZE
constexpr privilege_t PRIV_STMT_SHOW_EXPLAIN= PROCESS_ACL;
constexpr privilege_t PRIV_STMT_SHOW_ENGINE_STATUS= PROCESS_ACL;
constexpr privilege_t PRIV_STMT_SHOW_ENGINE_MUTEX= PROCESS_ACL;
constexpr privilege_t PRIV_STMT_SHOW_PROCESSLIST= PROCESS_ACL;


/*
  Defines to change the above bits to how things are stored in tables
  This is needed as the 'host' and 'db' table is missing a few privileges
*/

/* Privileges that need to be reallocated (in continous chunks) */
constexpr privilege_t DB_CHUNK0 (COL_DML_ACLS | CREATE_ACL | DROP_ACL);
constexpr privilege_t DB_CHUNK1 (GRANT_ACL | REFERENCES_ACL | INDEX_ACL | ALTER_ACL);
constexpr privilege_t DB_CHUNK2 (CREATE_TMP_ACL | LOCK_TABLES_ACL);
constexpr privilege_t DB_CHUNK3 (VIEW_ACLS | PROC_DDL_ACLS);
constexpr privilege_t DB_CHUNK4 (EXECUTE_ACL);
constexpr privilege_t DB_CHUNK5 (EVENT_ACL | TRIGGER_ACL);
constexpr privilege_t DB_CHUNK6 (DELETE_HISTORY_ACL);


static inline privilege_t fix_rights_for_db(privilege_t access)
{
  ulonglong A(access);
  return static_cast<privilege_t>
           (((A)      & DB_CHUNK0) |
            ((A << 4) & DB_CHUNK1) |
            ((A << 6) & DB_CHUNK2) |
            ((A << 9) & DB_CHUNK3) |
            ((A << 2) & DB_CHUNK4) |
            ((A << 9) & DB_CHUNK5) |
            ((A << 10) & DB_CHUNK6));
}

static inline privilege_t get_rights_for_db(privilege_t access)
{
  ulonglong A(access);
  return static_cast<privilege_t>
           ((A & DB_CHUNK0)       |
           ((A & DB_CHUNK1) >> 4) |
           ((A & DB_CHUNK2) >> 6) |
           ((A & DB_CHUNK3) >> 9) |
           ((A & DB_CHUNK4) >> 2) |
           ((A & DB_CHUNK5) >> 9) |
           ((A & DB_CHUNK6) >> 10));
}


#define TBL_CHUNK0 DB_CHUNK0
#define TBL_CHUNK1 DB_CHUNK1
#define TBL_CHUNK2 (CREATE_VIEW_ACL | SHOW_VIEW_ACL)
#define TBL_CHUNK3 TRIGGER_ACL
#define TBL_CHUNK4 (DELETE_HISTORY_ACL)


static inline privilege_t fix_rights_for_table(privilege_t access)
{
  ulonglong A(access);
  return static_cast<privilege_t>
           ((A        & TBL_CHUNK0) |
           ((A <<  4) & TBL_CHUNK1) |
           ((A << 11) & TBL_CHUNK2) |
           ((A << 15) & TBL_CHUNK3) |
           ((A << 16) & TBL_CHUNK4));
}


static inline privilege_t get_rights_for_table(privilege_t access)
{
  ulonglong A(access);
  return static_cast<privilege_t>
           ((A & TBL_CHUNK0)        |
           ((A & TBL_CHUNK1) >>  4) |
           ((A & TBL_CHUNK2) >> 11) |
           ((A & TBL_CHUNK3) >> 15) |
           ((A & TBL_CHUNK4) >> 16));
}


static inline privilege_t fix_rights_for_column(privilege_t A)
{
  const ulonglong mask(SELECT_ACL | INSERT_ACL | UPDATE_ACL);
  return (A & mask) | static_cast<privilege_t>((A & ~mask) << 8);
}


static inline privilege_t get_rights_for_column(privilege_t A)
{
  const ulonglong mask(SELECT_ACL | INSERT_ACL | UPDATE_ACL);
  return static_cast<privilege_t>((static_cast<ulonglong>(A) & mask) |
                                  (static_cast<ulonglong>(A) >> 8));
}


static inline privilege_t fix_rights_for_procedure(privilege_t access)
{
  ulonglong A(access);
  return static_cast<privilege_t>
           (((A << 18) & EXECUTE_ACL)    |
            ((A << 23) & ALTER_PROC_ACL) |
            ((A << 8) & GRANT_ACL));
}


static inline privilege_t get_rights_for_procedure(privilege_t access)
{
  ulonglong A(access);
  return static_cast<privilege_t>
           (((A & EXECUTE_ACL)    >> 18) |
            ((A & ALTER_PROC_ACL) >> 23) |
            ((A & GRANT_ACL) >> 8));
}


#endif /* PRIVILEGE_H_INCLUDED */
