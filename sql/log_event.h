/* Copyright (c) 2000, 2014, Oracle and/or its affiliates.
   Copyright (c) 2009, 2020, MariaDB Corporation.

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

/**
  @addtogroup Replication
  @{

  @file
  
  @brief Binary log event definitions.  This includes generic code
  common to all types of log events, as well as specific code for each
  type of log event.
*/


#ifndef _log_event_h
#define _log_event_h

#if defined(USE_PRAGMA_INTERFACE) && defined(MYSQL_SERVER)
#pragma interface			/* gcc class implementation */
#endif

#include <my_bitmap.h>
#include "rpl_constants.h"
#include <vector>
#include <string>
#include <functional>
#include <memory>
#include <map>

#ifdef MYSQL_CLIENT
#include "sql_const.h"
#include "rpl_utility.h"
#include "hash.h"
#include "rpl_tblmap.h"
#include "sql_string.h"
#endif

#ifdef MYSQL_SERVER
#include "rpl_record.h"
#include "rpl_reporting.h"
#include "sql_class.h"                          /* THD */
#endif

#include "rpl_gtid.h"

/* Forward declarations */
#ifndef MYSQL_CLIENT
class String;
#endif

#define PREFIX_SQL_LOAD "SQL_LOAD-"
#define LONG_FIND_ROW_THRESHOLD 60 /* seconds */

/**
   Either assert or return an error.

   In debug build, the condition will be checked, but in non-debug
   builds, the error code given will be returned instead.

   @param COND   Condition to check
   @param ERRNO  Error number to return in non-debug builds
*/
#ifdef DBUG_OFF
#define ASSERT_OR_RETURN_ERROR(COND, ERRNO) \
  do { if (!(COND)) return ERRNO; } while (0)
#else
#define ASSERT_OR_RETURN_ERROR(COND, ERRNO) \
  DBUG_ASSERT(COND)
#endif

#define LOG_READ_EOF    -1
#define LOG_READ_BOGUS  -2
#define LOG_READ_IO     -3
#define LOG_READ_MEM    -5
#define LOG_READ_TRUNC  -6
#define LOG_READ_TOO_LARGE -7
#define LOG_READ_CHECKSUM_FAILURE -8
#define LOG_READ_DECRYPT -9

#define LOG_EVENT_OFFSET 4

/*
   3 is MySQL 4.x; 4 is MySQL 5.0.0.
   Compared to version 3, version 4 has:
   - a different Start_log_event, which includes info about the binary log
   (sizes of headers); this info is included for better compatibility if the
   master's MySQL version is different from the slave's.
   - all events have a unique ID (the triplet (server_id, timestamp at server
   start, other) to be sure an event is not executed more than once in a
   multimaster setup, example:
                M1
              /   \
             v     v
             M2    M3
             \     /
              v   v
                S
   if a query is run on M1, it will arrive twice on S, so we need that S
   remembers the last unique ID it has processed, to compare and know if the
   event should be skipped or not. Example of ID: we already have the server id
   (4 bytes), plus:
   timestamp_when_the_master_started (4 bytes), a counter (a sequence number
   which increments every time we write an event to the binlog) (3 bytes).
   Q: how do we handle when the counter is overflowed and restarts from 0 ?

   - Query and Load (Create or Execute) events may have a more precise
     timestamp (with microseconds), number of matched/affected/warnings rows
   and fields of session variables: SQL_MODE,
   FOREIGN_KEY_CHECKS, UNIQUE_CHECKS, SQL_AUTO_IS_NULL, the collations and
   charsets, the PASSWORD() version (old/new/...).
*/
#define BINLOG_VERSION    4

/*
 We could have used SERVER_VERSION_LENGTH, but this introduces an
 obscure dependency - if somebody decided to change SERVER_VERSION_LENGTH
 this would break the replication protocol
*/
#define ST_SERVER_VER_LEN 50

/*
  These are flags and structs to handle all the LOAD DATA INFILE options (LINES
  TERMINATED etc).
*/

/*
  These are flags and structs to handle all the LOAD DATA INFILE options (LINES
  TERMINATED etc).
  DUMPFILE_FLAG is probably useless (DUMPFILE is a clause of SELECT, not of LOAD
  DATA).
*/
#define DUMPFILE_FLAG		0x1
#define OPT_ENCLOSED_FLAG	0x2
#define REPLACE_FLAG		0x4
#define IGNORE_FLAG		0x8

#define FIELD_TERM_EMPTY	0x1
#define ENCLOSED_EMPTY		0x2
#define LINE_TERM_EMPTY		0x4
#define LINE_START_EMPTY	0x8
#define ESCAPED_EMPTY		0x10

#define NUM_LOAD_DELIM_STRS 5

/*****************************************************************************

  MySQL Binary Log

  This log consists of events.  Each event has a fixed-length header,
  possibly followed by a variable length data body.

  The data body consists of an optional fixed length segment (post-header)
  and  an optional variable length segment.

  See the #defines below for the format specifics.

  The events which really update data are Query_log_event,
  Execute_load_query_log_event and old Load_log_event and
  Execute_load_log_event events (Execute_load_query is used together with
  Begin_load_query and Append_block events to replicate LOAD DATA INFILE.
  Create_file/Append_block/Execute_load (which includes Load_log_event)
  were used to replicate LOAD DATA before the 5.0.3).

 ****************************************************************************/

#define LOG_EVENT_HEADER_LEN 19     /* the fixed header length */
#define OLD_HEADER_LEN       13     /* the fixed header length in 3.23 */
/*
   Fixed header length, where 4.x and 5.0 agree. That is, 5.0 may have a longer
   header (it will for sure when we have the unique event's ID), but at least
   the first 19 bytes are the same in 4.x and 5.0. So when we have the unique
   event's ID, LOG_EVENT_HEADER_LEN will be something like 26, but
   LOG_EVENT_MINIMAL_HEADER_LEN will remain 19.
*/
#define LOG_EVENT_MINIMAL_HEADER_LEN 19

/* event-specific post-header sizes */
// where 3.23, 4.x and 5.0 agree
#define QUERY_HEADER_MINIMAL_LEN     (4 + 4 + 1 + 2)
// where 5.0 differs: 2 for len of N-bytes vars.
#define QUERY_HEADER_LEN     (QUERY_HEADER_MINIMAL_LEN + 2)
#define STOP_HEADER_LEN      0
#define LOAD_HEADER_LEN      (4 + 4 + 4 + 1 +1 + 4)
#define SLAVE_HEADER_LEN     0
#define START_V3_HEADER_LEN     (2 + ST_SERVER_VER_LEN + 4)
#define ROTATE_HEADER_LEN    8 // this is FROZEN (the Rotate post-header is frozen)
#define INTVAR_HEADER_LEN      0
#define CREATE_FILE_HEADER_LEN 4
#define APPEND_BLOCK_HEADER_LEN 4
#define EXEC_LOAD_HEADER_LEN   4
#define DELETE_FILE_HEADER_LEN 4
#define NEW_LOAD_HEADER_LEN    LOAD_HEADER_LEN
#define RAND_HEADER_LEN        0
#define USER_VAR_HEADER_LEN    0
#define FORMAT_DESCRIPTION_HEADER_LEN (START_V3_HEADER_LEN+1+LOG_EVENT_TYPES)
#define XID_HEADER_LEN         0
#define BEGIN_LOAD_QUERY_HEADER_LEN APPEND_BLOCK_HEADER_LEN
#define ROWS_HEADER_LEN_V1     8
#define TABLE_MAP_HEADER_LEN   8
#define EXECUTE_LOAD_QUERY_EXTRA_HEADER_LEN (4 + 4 + 4 + 1)
#define EXECUTE_LOAD_QUERY_HEADER_LEN  (QUERY_HEADER_LEN + EXECUTE_LOAD_QUERY_EXTRA_HEADER_LEN)
#define INCIDENT_HEADER_LEN    2
#define HEARTBEAT_HEADER_LEN   0
#define IGNORABLE_HEADER_LEN   0
#define ROWS_HEADER_LEN_V2    10
#define ANNOTATE_ROWS_HEADER_LEN  0
#define BINLOG_CHECKPOINT_HEADER_LEN 4
#define GTID_HEADER_LEN       19
#define GTID_LIST_HEADER_LEN   4
#define START_ENCRYPTION_HEADER_LEN 0
#define XA_PREPARE_HEADER_LEN 0

/* 
  Max number of possible extra bytes in a replication event compared to a
  packet (i.e. a query) sent from client to master;
  First, an auxiliary log_event status vars estimation:
*/
#define MAX_SIZE_LOG_EVENT_STATUS (1 + 4          /* type, flags2 */   + \
                                   1 + 8          /* type, sql_mode */ + \
                                   1 + 1 + 255    /* type, length, catalog */ + \
                                   1 + 4          /* type, auto_increment */ + \
                                   1 + 6          /* type, charset */ + \
                                   1 + 1 + 255    /* type, length, time_zone */ + \
                                   1 + 2          /* type, lc_time_names_number */ + \
                                   1 + 2          /* type, charset_database_number */ + \
                                   1 + 8          /* type, table_map_for_update */ + \
                                   1 + 4          /* type, master_data_written */ + \
                                   1 + 3          /* type, sec_part of NOW() */ + \
                                   1 + 16 + 1 + 60/* type, user_len, user, host_len, host */)
#define MAX_LOG_EVENT_HEADER   ( /* in order of Query_log_event::write */ \
  LOG_EVENT_HEADER_LEN + /* write_header */ \
  QUERY_HEADER_LEN     + /* write_data */   \
  EXECUTE_LOAD_QUERY_EXTRA_HEADER_LEN + /*write_post_header_for_derived */ \
  MAX_SIZE_LOG_EVENT_STATUS + /* status */ \
  NAME_LEN + 1)

/*
  The new option is added to handle large packets that are sent from the master 
  to the slave. It is used to increase the thd(max_allowed) for both the
  DUMP thread on the master and the SQL/IO thread on the slave. 
*/
#define MAX_MAX_ALLOWED_PACKET (1024*1024*1024)

/* 
   Event header offsets; 
   these point to places inside the fixed header.
*/

#define EVENT_TYPE_OFFSET    4
#define SERVER_ID_OFFSET     5
#define EVENT_LEN_OFFSET     9
#define LOG_POS_OFFSET       13
#define FLAGS_OFFSET         17

/* start event post-header (for v3 and v4) */

#define ST_BINLOG_VER_OFFSET  0
#define ST_SERVER_VER_OFFSET  2
#define ST_CREATED_OFFSET     (ST_SERVER_VER_OFFSET + ST_SERVER_VER_LEN)
#define ST_COMMON_HEADER_LEN_OFFSET (ST_CREATED_OFFSET + 4)

/* slave event post-header (this event is never written) */

#define SL_MASTER_PORT_OFFSET   8
#define SL_MASTER_POS_OFFSET    0
#define SL_MASTER_HOST_OFFSET   10

/* query event post-header */

#define Q_THREAD_ID_OFFSET	0
#define Q_EXEC_TIME_OFFSET	4
#define Q_DB_LEN_OFFSET		8
#define Q_ERR_CODE_OFFSET	9
#define Q_STATUS_VARS_LEN_OFFSET 11
#define Q_DATA_OFFSET		QUERY_HEADER_LEN
/* these are codes, not offsets; not more than 256 values (1 byte). */
#define Q_FLAGS2_CODE           0
#define Q_SQL_MODE_CODE         1
/*
  Q_CATALOG_CODE is catalog with end zero stored; it is used only by MySQL
  5.0.x where 0<=x<=3. We have to keep it to be able to replicate these
  old masters.
*/
#define Q_CATALOG_CODE          2
#define Q_AUTO_INCREMENT	3
#define Q_CHARSET_CODE          4
#define Q_TIME_ZONE_CODE        5
/*
  Q_CATALOG_NZ_CODE is catalog withOUT end zero stored; it is used by MySQL
  5.0.x where x>=4. Saves one byte in every Query_log_event in binlog,
  compared to Q_CATALOG_CODE. The reason we didn't simply re-use
  Q_CATALOG_CODE is that then a 5.0.3 slave of this 5.0.x (x>=4) master would
  crash (segfault etc) because it would expect a 0 when there is none.
*/
#define Q_CATALOG_NZ_CODE       6

#define Q_LC_TIME_NAMES_CODE    7

#define Q_CHARSET_DATABASE_CODE 8

#define Q_TABLE_MAP_FOR_UPDATE_CODE 9

#define Q_MASTER_DATA_WRITTEN_CODE 10

#define Q_INVOKER 11

#define Q_HRNOW 128
#define Q_XID   129

/* Intvar event post-header */

/* Intvar event data */
#define I_TYPE_OFFSET        0
#define I_VAL_OFFSET         1

/* Rand event data */
#define RAND_SEED1_OFFSET 0
#define RAND_SEED2_OFFSET 8

/* User_var event data */
#define UV_VAL_LEN_SIZE        4
#define UV_VAL_IS_NULL         1
#define UV_VAL_TYPE_SIZE       1
#define UV_NAME_LEN_SIZE       4
#define UV_CHARSET_NUMBER_SIZE 4

/* Load event post-header */
#define L_THREAD_ID_OFFSET   0
#define L_EXEC_TIME_OFFSET   4
#define L_SKIP_LINES_OFFSET  8
#define L_TBL_LEN_OFFSET     12
#define L_DB_LEN_OFFSET      13
#define L_NUM_FIELDS_OFFSET  14
#define L_SQL_EX_OFFSET      18
#define L_DATA_OFFSET        LOAD_HEADER_LEN

/* Rotate event post-header */
#define R_POS_OFFSET       0
#define R_IDENT_OFFSET     8

/* CF to DF handle LOAD DATA INFILE */

/* CF = "Create File" */
#define CF_FILE_ID_OFFSET  0
#define CF_DATA_OFFSET     CREATE_FILE_HEADER_LEN

/* AB = "Append Block" */
#define AB_FILE_ID_OFFSET  0
#define AB_DATA_OFFSET     APPEND_BLOCK_HEADER_LEN

/* EL = "Execute Load" */
#define EL_FILE_ID_OFFSET  0

/* DF = "Delete File" */
#define DF_FILE_ID_OFFSET  0

/* TM = "Table Map" */
#define TM_MAPID_OFFSET    0
#define TM_FLAGS_OFFSET    6

/* RW = "RoWs" */
#define RW_MAPID_OFFSET    0
#define RW_FLAGS_OFFSET    6
#define RW_VHLEN_OFFSET    8
#define RW_V_TAG_LEN       1
#define RW_V_EXTRAINFO_TAG 0

/* ELQ = "Execute Load Query" */
#define ELQ_FILE_ID_OFFSET QUERY_HEADER_LEN
#define ELQ_FN_POS_START_OFFSET ELQ_FILE_ID_OFFSET + 4
#define ELQ_FN_POS_END_OFFSET ELQ_FILE_ID_OFFSET + 8
#define ELQ_DUP_HANDLING_OFFSET ELQ_FILE_ID_OFFSET + 12

/* 4 bytes which all binlogs should begin with */
#define BINLOG_MAGIC        (const uchar*) "\xfe\x62\x69\x6e"

/*
  The 2 flags below were useless :
  - the first one was never set
  - the second one was set in all Rotate events on the master, but not used for
  anything useful.
  So they are now removed and their place may later be reused for other
  flags. Then one must remember that Rotate events in 4.x have
  LOG_EVENT_FORCED_ROTATE_F set, so one should not rely on the value of the
  replacing flag when reading a Rotate event.
  I keep the defines here just to remember what they were.
*/
#ifdef TO_BE_REMOVED
#define LOG_EVENT_TIME_F            0x1
#define LOG_EVENT_FORCED_ROTATE_F   0x2
#endif

/*
   This flag only makes sense for Format_description_log_event. It is set
   when the event is written, and *reset* when a binlog file is
   closed (yes, it's the only case when MySQL modifies already written
   part of binlog).  Thus it is a reliable indicator that binlog was
   closed correctly.  (Stop_log_event is not enough, there's always a
   small chance that mysqld crashes in the middle of insert and end of
   the binlog would look like a Stop_log_event).

   This flag is used to detect a restart after a crash, and to provide
   "unbreakable" binlog. The problem is that on a crash storage engines
   rollback automatically, while binlog does not.  To solve this we use this
   flag and automatically append ROLLBACK to every non-closed binlog (append
   virtually, on reading, file itself is not changed). If this flag is found,
   mysqlbinlog simply prints "ROLLBACK" Replication master does not abort on
   binlog corruption, but takes it as EOF, and replication slave forces a
   rollback in this case.

   Note, that old binlogs does not have this flag set, so we get a
   a backward-compatible behaviour.
*/

#define LOG_EVENT_BINLOG_IN_USE_F       0x1

/**
  @def LOG_EVENT_THREAD_SPECIFIC_F

  If the query depends on the thread (for example: TEMPORARY TABLE).
  Currently this is used by mysqlbinlog to know it must print
  SET @@PSEUDO_THREAD_ID=xx; before the query (it would not hurt to print it
  for every query but this would be slow).
*/
#define LOG_EVENT_THREAD_SPECIFIC_F 0x4

/**
  @def LOG_EVENT_SUPPRESS_USE_F

  Suppress the generation of 'USE' statements before the actual
  statement. This flag should be set for any events that does not need
  the current database set to function correctly. Most notable cases
  are 'CREATE DATABASE' and 'DROP DATABASE'.

  This flags should only be used in exceptional circumstances, since
  it introduce a significant change in behaviour regarding the
  replication logic together with the flags --binlog-do-db and
  --replicated-do-db.
 */
#define LOG_EVENT_SUPPRESS_USE_F    0x8

/*
  Note: this is a place holder for the flag
  LOG_EVENT_UPDATE_TABLE_MAP_VERSION_F (0x10), which is not used any
  more, please do not reused this value for other flags.
 */

/**
   @def LOG_EVENT_ARTIFICIAL_F
   
   Artificial events are created arbitrarily and not written to binary
   log

   These events should not update the master log position when slave
   SQL thread executes them.
*/
#define LOG_EVENT_ARTIFICIAL_F 0x20

/**
   @def LOG_EVENT_RELAY_LOG_F
   
   Events with this flag set are created by slave IO thread and written
   to relay log
*/
#define LOG_EVENT_RELAY_LOG_F 0x40

/**
   @def LOG_EVENT_IGNORABLE_F

   For an event, 'e', carrying a type code, that a slave,
   's', does not recognize, 's' will check 'e' for
   LOG_EVENT_IGNORABLE_F, and if the flag is set, then 'e'
   is ignored. Otherwise, 's' acknowledges that it has
   found an unknown event in the relay log.
*/
#define LOG_EVENT_IGNORABLE_F 0x80

/**
   @def LOG_EVENT_ACCEPT_OWN_F

   Flag sets by the semisync slave for accepting
   the same server_id ("own") events which the slave must not have
   in its state. Typically such events were never committed by
   their originator (this server) and discared at its semisync-slave recovery.
*/
#define LOG_EVENT_ACCEPT_OWN_F 0x4000

/**
   @def LOG_EVENT_SKIP_REPLICATION_F

   Flag set by application creating the event (with @@skip_replication); the
   slave will skip replication of such events if
   --replicate-events-marked-for-skip is not set to REPLICATE.

   This is a MariaDB flag; we allocate it from the end of the available
   values to reduce risk of conflict with new MySQL flags.
*/
#define LOG_EVENT_SKIP_REPLICATION_F 0x8000


/**
  @def OPTIONS_WRITTEN_TO_BIN_LOG

  OPTIONS_WRITTEN_TO_BIN_LOG are the bits of thd->options which must
  be written to the binlog. OPTIONS_WRITTEN_TO_BIN_LOG could be
  written into the Format_description_log_event, so that if later we
  don't want to replicate a variable we did replicate, or the
  contrary, it's doable. But it should not be too hard to decide once
  for all of what we replicate and what we don't, among the fixed 32
  bits of thd->options.

  I (Guilhem) have read through every option's usage, and it looks
  like OPTION_AUTO_IS_NULL and OPTION_NO_FOREIGN_KEYS are the only
  ones which alter how the query modifies the table. It's good to
  replicate OPTION_RELAXED_UNIQUE_CHECKS too because otherwise, the
  slave may insert data slower than the master, in InnoDB.
  OPTION_BIG_SELECTS is not needed (the slave thread runs with
  max_join_size=HA_POS_ERROR) and OPTION_BIG_TABLES is not needed
  either, as the manual says (because a too big in-memory temp table
  is automatically written to disk).
*/
#define OPTIONS_WRITTEN_TO_BIN_LOG \
  (OPTION_AUTO_IS_NULL | OPTION_NO_FOREIGN_KEY_CHECKS |  \
   OPTION_RELAXED_UNIQUE_CHECKS | OPTION_NOT_AUTOCOMMIT | OPTION_IF_EXISTS)

/* Shouldn't be defined before */
#define EXPECTED_OPTIONS \
  ((1ULL << 14) | (1ULL << 26) | (1ULL << 27) | (1ULL << 19) | (1ULL << 28))

#if OPTIONS_WRITTEN_TO_BIN_LOG != EXPECTED_OPTIONS
#error OPTIONS_WRITTEN_TO_BIN_LOG must NOT change their values!
#endif
#undef EXPECTED_OPTIONS         /* You shouldn't use this one */

#define CHECKSUM_CRC32_SIGNATURE_LEN 4
/**
   defined statically while there is just one alg implemented
*/
#define BINLOG_CHECKSUM_LEN CHECKSUM_CRC32_SIGNATURE_LEN
#define BINLOG_CHECKSUM_ALG_DESC_LEN 1  /* 1 byte checksum alg descriptor */

/*
  These are capability numbers for MariaDB slave servers.

  Newer MariaDB slaves set this to inform the master about their capabilities.
  This allows the master to decide which events it can send to the slave
  without breaking replication on old slaves that maybe do not understand
  all events from newer masters.

  As new releases are backwards compatible, a given capability implies also
  all capabilities with smaller number.

  Older MariaDB slaves and other MySQL slave servers do not set this, so they
  are recorded with capability 0.
*/

/* MySQL or old MariaDB slave with no announced capability. */
#define MARIA_SLAVE_CAPABILITY_UNKNOWN 0
/* MariaDB >= 5.3, which understands ANNOTATE_ROWS_EVENT. */
#define MARIA_SLAVE_CAPABILITY_ANNOTATE 1
/*
  MariaDB >= 5.5. This version has the capability to tolerate events omitted
  from the binlog stream without breaking replication (MySQL slaves fail
  because they mis-compute the offsets into the master's binlog).
*/
#define MARIA_SLAVE_CAPABILITY_TOLERATE_HOLES 2
/* MariaDB >= 10.0, which knows about binlog_checkpoint_log_event. */
#define MARIA_SLAVE_CAPABILITY_BINLOG_CHECKPOINT 3
/* MariaDB >= 10.0.1, which knows about global transaction id events. */
#define MARIA_SLAVE_CAPABILITY_GTID 4

/* Our capability. */
#define MARIA_SLAVE_CAPABILITY_MINE MARIA_SLAVE_CAPABILITY_GTID


/*
  When the size of 'log_pos' within Heartbeat_log_event exceeds UINT32_MAX it
  cannot be accommodated in common_header, as 'log_pos' is of 4 bytes size. In
  such cases, sub_header, of size 8 bytes will hold larger 'log_pos' value.
*/
#define HB_SUB_HEADER_LEN 8


/**
  @enum Log_event_type

  Enumeration type for the different types of log events.
*/
enum Log_event_type
{
  /*
    Every time you update this enum (when you add a type), you have to
    fix Format_description_log_event::Format_description_log_event().
  */
  UNKNOWN_EVENT= 0,
  START_EVENT_V3= 1,
  QUERY_EVENT= 2,
  STOP_EVENT= 3,
  ROTATE_EVENT= 4,
  INTVAR_EVENT= 5,
  LOAD_EVENT= 6,
  SLAVE_EVENT= 7,
  CREATE_FILE_EVENT= 8,
  APPEND_BLOCK_EVENT= 9,
  EXEC_LOAD_EVENT= 10,
  DELETE_FILE_EVENT= 11,
  /*
    NEW_LOAD_EVENT is like LOAD_EVENT except that it has a longer
    sql_ex, allowing multibyte TERMINATED BY etc; both types share the
    same class (Load_log_event)
  */
  NEW_LOAD_EVENT= 12,
  RAND_EVENT= 13,
  USER_VAR_EVENT= 14,
  FORMAT_DESCRIPTION_EVENT= 15,
  XID_EVENT= 16,
  BEGIN_LOAD_QUERY_EVENT= 17,
  EXECUTE_LOAD_QUERY_EVENT= 18,

  TABLE_MAP_EVENT = 19,

  /*
    These event numbers were used for 5.1.0 to 5.1.15 and are
    therefore obsolete.
   */
  PRE_GA_WRITE_ROWS_EVENT = 20,
  PRE_GA_UPDATE_ROWS_EVENT = 21,
  PRE_GA_DELETE_ROWS_EVENT = 22,

  /*
    These event numbers are used from 5.1.16 until mysql-5.6.6,
    and in MariaDB
   */
  WRITE_ROWS_EVENT_V1 = 23,
  UPDATE_ROWS_EVENT_V1 = 24,
  DELETE_ROWS_EVENT_V1 = 25,

  /*
    Something out of the ordinary happened on the master
   */
  INCIDENT_EVENT= 26,

  /*
    Heartbeat event to be send by master at its idle time 
    to ensure master's online status to slave 
  */
  HEARTBEAT_LOG_EVENT= 27,
  
  /*
    In some situations, it is necessary to send over ignorable
    data to the slave: data that a slave can handle in case there
    is code for handling it, but which can be ignored if it is not
    recognized.

    These mysql-5.6 events are not recognized (and ignored) by MariaDB
  */
  IGNORABLE_LOG_EVENT= 28,
  ROWS_QUERY_LOG_EVENT= 29,
 
  /* Version 2 of the Row events, generated only by mysql-5.6.6+ */
  WRITE_ROWS_EVENT = 30,
  UPDATE_ROWS_EVENT = 31,
  DELETE_ROWS_EVENT = 32,
 
  /* MySQL 5.6 GTID events, ignored by MariaDB */
  GTID_LOG_EVENT= 33,
  ANONYMOUS_GTID_LOG_EVENT= 34,
  PREVIOUS_GTIDS_LOG_EVENT= 35,

  /* MySQL 5.7 events, ignored by MariaDB */
  TRANSACTION_CONTEXT_EVENT= 36,
  VIEW_CHANGE_EVENT= 37,
  /* not ignored */
  XA_PREPARE_LOG_EVENT= 38,

  /*
    Add new events here - right above this comment!
    Existing events (except ENUM_END_EVENT) should never change their numbers
  */

  /* New MySQL/Sun events are to be added right above this comment */
  MYSQL_EVENTS_END,

  MARIA_EVENTS_BEGIN= 160,
  /* New Maria event numbers start from here */
  ANNOTATE_ROWS_EVENT= 160,
  /*
    Binlog checkpoint event. Used for XA crash recovery on the master, not used
    in replication.
    A binlog checkpoint event specifies a binlog file such that XA crash
    recovery can start from that file - and it is guaranteed to find all XIDs
    that are prepared in storage engines but not yet committed.
  */
  BINLOG_CHECKPOINT_EVENT= 161,
  /*
    Gtid event. For global transaction ID, used to start a new event group,
    instead of the old BEGIN query event, and also to mark stand-alone
    events.
  */
  GTID_EVENT= 162,
  /*
    Gtid list event. Logged at the start of every binlog, to record the
    current replication state. This consists of the last GTID seen for
    each replication domain.
  */
  GTID_LIST_EVENT= 163,

  START_ENCRYPTION_EVENT= 164,

  /*
    Compressed binlog event.

    Note that the order between WRITE/UPDATE/DELETE events is significant;
    this is so that we can convert from the compressed to the uncompressed
    event type with (type-WRITE_ROWS_COMPRESSED_EVENT + WRITE_ROWS_EVENT)
    and similar for _V1.
  */
  QUERY_COMPRESSED_EVENT = 165,
  WRITE_ROWS_COMPRESSED_EVENT_V1 = 166,
  UPDATE_ROWS_COMPRESSED_EVENT_V1 = 167,
  DELETE_ROWS_COMPRESSED_EVENT_V1 = 168,
  WRITE_ROWS_COMPRESSED_EVENT = 169,
  UPDATE_ROWS_COMPRESSED_EVENT = 170,
  DELETE_ROWS_COMPRESSED_EVENT = 171,

  /* Add new MariaDB events here - right above this comment!  */

  ENUM_END_EVENT /* end marker */
};


/*
  Bit flags for what has been writting to cache. Used to
  discard logs with table map events but not row events and
  nothing else important. This is stored by cache.
*/

enum enum_logged_status
{
  LOGGED_TABLE_MAP= 1,
  LOGGED_ROW_EVENT= 2,
  LOGGED_NO_DATA=   4,
  LOGGED_CRITICAL=  8
};

static inline bool LOG_EVENT_IS_QUERY(enum Log_event_type type)
{
  return type == QUERY_EVENT || type == QUERY_COMPRESSED_EVENT;
}


static inline bool LOG_EVENT_IS_WRITE_ROW(enum Log_event_type type)
{
  return type == WRITE_ROWS_EVENT || type == WRITE_ROWS_EVENT_V1 ||
    type == WRITE_ROWS_COMPRESSED_EVENT ||
    type == WRITE_ROWS_COMPRESSED_EVENT_V1;
}


static inline bool LOG_EVENT_IS_UPDATE_ROW(enum Log_event_type type)
{
  return type == UPDATE_ROWS_EVENT || type == UPDATE_ROWS_EVENT_V1 ||
    type == UPDATE_ROWS_COMPRESSED_EVENT ||
    type == UPDATE_ROWS_COMPRESSED_EVENT_V1;
}


static inline bool LOG_EVENT_IS_DELETE_ROW(enum Log_event_type type)
{
  return type == DELETE_ROWS_EVENT || type == DELETE_ROWS_EVENT_V1 ||
    type == DELETE_ROWS_COMPRESSED_EVENT ||
    type == DELETE_ROWS_COMPRESSED_EVENT_V1;
}


static inline bool LOG_EVENT_IS_ROW_COMPRESSED(enum Log_event_type type)
{
  return type == WRITE_ROWS_COMPRESSED_EVENT ||
    type == WRITE_ROWS_COMPRESSED_EVENT_V1 ||
    type == UPDATE_ROWS_COMPRESSED_EVENT ||
    type == UPDATE_ROWS_COMPRESSED_EVENT_V1 ||
    type == DELETE_ROWS_COMPRESSED_EVENT ||
    type == DELETE_ROWS_COMPRESSED_EVENT_V1;
}


static inline bool LOG_EVENT_IS_ROW_V2(enum Log_event_type type)
{
  return (type >= WRITE_ROWS_EVENT && type <= DELETE_ROWS_EVENT) ||
    (type >= WRITE_ROWS_COMPRESSED_EVENT && type <= DELETE_ROWS_COMPRESSED_EVENT);
}


/*
   The number of types we handle in Format_description_log_event (UNKNOWN_EVENT
   is not to be handled, it does not exist in binlogs, it does not have a
   format).
*/
#define LOG_EVENT_TYPES (ENUM_END_EVENT-1)

enum Int_event_type
{
  INVALID_INT_EVENT = 0, LAST_INSERT_ID_EVENT = 1, INSERT_ID_EVENT = 2
};

#ifdef MYSQL_SERVER
class String;
class MYSQL_BIN_LOG;
class THD;
#endif

class Format_description_log_event;
class Relay_log_info;
class binlog_cache_data;

bool copy_event_cache_to_file_and_reinit(IO_CACHE *cache, FILE *file);

#ifdef MYSQL_CLIENT
enum enum_base64_output_mode {
  BASE64_OUTPUT_NEVER= 0,
  BASE64_OUTPUT_AUTO= 1,
  BASE64_OUTPUT_UNSPEC= 2,
  BASE64_OUTPUT_DECODE_ROWS= 3,
  /* insert new output modes here */
  BASE64_OUTPUT_MODE_COUNT
};

bool copy_event_cache_to_string_and_reinit(IO_CACHE *cache, LEX_STRING *to);

/*
  A structure for mysqlbinlog to know how to print events

  This structure is passed to the event's print() methods,

  There are two types of settings stored here:
  1. Last db, flags2, sql_mode etc comes from the last printed event.
     They are stored so that only the necessary USE and SET commands
     are printed.
  2. Other information on how to print the events, e.g. short_form,
     hexdump_from.  These are not dependent on the last event.
*/
typedef struct st_print_event_info
{
  /*
    Settings for database, sql_mode etc that comes from the last event
    that was printed.  We cache these so that we don't have to print
    them if they are unchanged.
  */
  char db[FN_REFLEN+1]; // TODO: make this a LEX_STRING when thd->db is
  char charset[6]; // 3 variables, each of them storable in 2 bytes
  char time_zone_str[MAX_TIME_ZONE_NAME_LENGTH];
  char delimiter[16];
  sql_mode_t sql_mode;		/* must be same as THD.variables.sql_mode */
  my_thread_id thread_id;
  ulonglong row_events;
  ulong auto_increment_increment, auto_increment_offset;
  uint lc_time_names_number;
  uint charset_database_number;
  uint verbose;
  uint32 flags2;
  uint32 server_id;
  uint32 domain_id;
  uint8 common_header_len;
  enum_base64_output_mode base64_output_mode;
  my_off_t hexdump_from;

  table_mapping m_table_map;
  table_mapping m_table_map_ignored;
  bool flags2_inited;
  bool sql_mode_inited;
  bool charset_inited;
  bool thread_id_printed;
  bool server_id_printed;
  bool domain_id_printed;
  bool allow_parallel;
  bool allow_parallel_printed;
  bool found_row_event;
  bool print_row_count;
  static const uint max_delimiter_size= 16;
  /* Settings on how to print the events */
  bool short_form;
  /*
    This is set whenever a Format_description_event is printed.
    Later, when an event is printed in base64, this flag is tested: if
    no Format_description_event has been seen, it is unsafe to print
    the base64 event, so an error message is generated.
  */
  bool printed_fd_event;
  /*
    Track when @@skip_replication changes so we need to output a SET
    statement for it.
  */
  bool skip_replication;
  bool print_table_metadata;

  /*
     These two caches are used by the row-based replication events to
     collect the header information and the main body of the events
     making up a statement.
   */
  IO_CACHE head_cache;
  IO_CACHE body_cache;
  IO_CACHE tail_cache;
#ifdef WHEN_FLASHBACK_REVIEW_READY
  /* Storing the SQL for reviewing */
  IO_CACHE review_sql_cache;
#endif
  FILE *file;
  st_print_event_info();

  ~st_print_event_info() {
    close_cached_file(&head_cache);
    close_cached_file(&body_cache);
    close_cached_file(&tail_cache);
#ifdef WHEN_FLASHBACK_REVIEW_READY
    close_cached_file(&review_sql_cache);
#endif
  }
  bool init_ok() /* tells if construction was successful */
    { return my_b_inited(&head_cache) && my_b_inited(&body_cache)
#ifdef WHEN_FLASHBACK_REVIEW_READY
      && my_b_inited(&review_sql_cache)
#endif
    ; }
  void flush_for_error()
  {
    if (!copy_event_cache_to_file_and_reinit(&head_cache, file))
      copy_event_cache_to_file_and_reinit(&body_cache, file);
    fflush(file);
  }
} PRINT_EVENT_INFO;
#endif

/**
  This class encapsulates writing of Log_event objects to IO_CACHE.
  Automatically calculates the checksum and encrypts the data, if necessary.
*/

class Log_event_writer
{
  /* Log_event_writer is updated when ctx is set */
  int (Log_event_writer::*encrypt_or_write)(const uchar *pos, size_t len);
public:
  ulonglong bytes_written;
  void *ctx;         ///< Encryption context or 0 if no encryption is needed
  uint checksum_len;
  int write(Log_event *ev);
  int write_header(uchar *pos, size_t len);
  int write_data(const uchar *pos, size_t len);
  int write_footer();
  my_off_t pos() { return my_b_safe_tell(file); }
  void add_status(enum_logged_status status);
  void set_incident();
  void set_encrypted_writer()
  { encrypt_or_write= &Log_event_writer::encrypt_and_write; }

  Log_event_writer(IO_CACHE *file_arg, binlog_cache_data *cache_data_arg,
                   Binlog_crypt_data *cr= 0)
    :encrypt_or_write(&Log_event_writer::write_internal),
    bytes_written(0), ctx(0),
    file(file_arg), cache_data(cache_data_arg), crypto(cr) { }

private:
  IO_CACHE *file;
  binlog_cache_data *cache_data;
  /**
    Placeholder for event checksum while writing to binlog.
   */
  ha_checksum crc;
  /**
    Encryption data (key, nonce). Only used if ctx != 0.
  */
  Binlog_crypt_data *crypto;
  /**
    Event length to be written into the next encrypted block
  */
  uint event_len;
  int write_internal(const uchar *pos, size_t len);
  int encrypt_and_write(const uchar *pos, size_t len);
  int maybe_write_event_len(uchar *pos, size_t len);
};

/**
  the struct aggregates two parameters that identify an event
  uniquely in scope of communication of a particular master and slave couple.
  I.e there can not be 2 events from the same staying connected master which
  have the same coordinates.
  @note
  Such identifier is not yet unique generally as the event originating master
  is resettable. Also the crashed master can be replaced with some other.
*/
typedef struct event_coordinates
{
  char * file_name; // binlog file name (directories stripped)
  my_off_t  pos;       // event's position in the binlog file
} LOG_POS_COORD;

/**
  @class Log_event

  This is the abstract base class for binary log events.
  
  @section Log_event_binary_format Binary Format

  Any @c Log_event saved on disk consists of the following three
  components.

  - Common-Header
  - Post-Header
  - Body

  The Common-Header, documented in the table @ref Table_common_header
  "below", always has the same form and length within one version of
  MySQL.  Each event type specifies a format and length of the
  Post-Header.  The length of the Common-Header is the same for all
  events of the same type.  The Body may be of different format and
  length even for different events of the same type.  The binary
  formats of Post-Header and Body are documented separately in each
  subclass.  The binary format of Common-Header is as follows.

  <table>
  <caption>Common-Header</caption>

  <tr>
    <th>Name</th>
    <th>Format</th>
    <th>Description</th>
  </tr>

  <tr>
    <td>timestamp</td>
    <td>4 byte unsigned integer</td>
    <td>The time when the query started, in seconds since 1970.
    </td>
  </tr>

  <tr>
    <td>type</td>
    <td>1 byte enumeration</td>
    <td>See enum #Log_event_type.</td>
  </tr>

  <tr>
    <td>server_id</td>
    <td>4 byte unsigned integer</td>
    <td>Server ID of the server that created the event.</td>
  </tr>

  <tr>
    <td>total_size</td>
    <td>4 byte unsigned integer</td>
    <td>The total size of this event, in bytes.  In other words, this
    is the sum of the sizes of Common-Header, Post-Header, and Body.
    </td>
  </tr>

  <tr>
    <td>master_position</td>
    <td>4 byte unsigned integer</td>
    <td>The position of the next event in the master binary log, in
    bytes from the beginning of the file.  In a binlog that is not a
    relay log, this is just the position of the next event, in bytes
    from the beginning of the file.  In a relay log, this is
    the position of the next event in the master's binlog.
    </td>
  </tr>

  <tr>
    <td>flags</td>
    <td>2 byte bitfield</td>
    <td>See Log_event::flags.</td>
  </tr>
  </table>

  Summing up the numbers above, we see that the total size of the
  common header is 19 bytes.

  @subsection Log_event_format_of_atomic_primitives Format of Atomic Primitives

  - All numbers, whether they are 16-, 24-, 32-, or 64-bit numbers,
  are stored in little endian, i.e., the least significant byte first,
  unless otherwise specified.

  @anchor packed_integer
  - Some events use a special format for efficient representation of
  unsigned integers, called Packed Integer.  A Packed Integer has the
  capacity of storing up to 8-byte integers, while small integers
  still can use 1, 3, or 4 bytes.  The value of the first byte
  determines how to read the number, according to the following table:

  <table>
  <caption>Format of Packed Integer</caption>

  <tr>
    <th>First byte</th>
    <th>Format</th>
  </tr>

  <tr>
    <td>0-250</td>
    <td>The first byte is the number (in the range 0-250), and no more
    bytes are used.</td>
  </tr>

  <tr>
    <td>252</td>
    <td>Two more bytes are used.  The number is in the range
    251-0xffff.</td>
  </tr>

  <tr>
    <td>253</td>
    <td>Three more bytes are used.  The number is in the range
    0xffff-0xffffff.</td>
  </tr>

  <tr>
    <td>254</td>
    <td>Eight more bytes are used.  The number is in the range
    0xffffff-0xffffffffffffffff.</td>
  </tr>

  </table>

  - Strings are stored in various formats.  The format of each string
  is documented separately.
*/
class Log_event
{
public:
  /**
     Enumeration of what kinds of skipping (and non-skipping) that can
     occur when the slave executes an event.

     @see shall_skip
     @see do_shall_skip
   */
  enum enum_skip_reason {
    /**
       Don't skip event.
    */
    EVENT_SKIP_NOT,

    /**
       Skip event by ignoring it.

       This means that the slave skip counter will not be changed.
    */
    EVENT_SKIP_IGNORE,

    /**
       Skip event and decrease skip counter.
    */
    EVENT_SKIP_COUNT
  };

  enum enum_event_cache_type 
  {
    EVENT_INVALID_CACHE,
    /* 
      If possible the event should use a non-transactional cache before
      being flushed to the binary log. This means that it must be flushed
      right after its correspondent statement is completed.
    */
    EVENT_STMT_CACHE,
    /* 
      The event should use a transactional cache before being flushed to
      the binary log. This means that it must be flushed upon commit or 
      rollback. 
    */
    EVENT_TRANSACTIONAL_CACHE,
    /* 
      The event must be written directly to the binary log without going
      through a cache.
    */
    EVENT_NO_CACHE,
    /**
       If there is a need for different types, introduce them before this.
    */
    EVENT_CACHE_COUNT
  };

  /*
    The following type definition is to be used whenever data is placed 
    and manipulated in a common buffer. Use this typedef for buffers
    that contain data containing binary and character data.
  */
  typedef unsigned char Byte;

  /*
    The offset in the log where this event originally appeared (it is
    preserved in relay logs, making SHOW SLAVE STATUS able to print
    coordinates of the event in the master's binlog). Note: when a
    transaction is written by the master to its binlog (wrapped in
    BEGIN/COMMIT) the log_pos of all the queries it contains is the
    one of the BEGIN (this way, when one does SHOW SLAVE STATUS it
    sees the offset of the BEGIN, which is logical as rollback may
    occur), except the COMMIT query which has its real offset.
  */
  my_off_t log_pos;
  /*
     A temp buffer for read_log_event; it is later analysed according to the
     event's type, and its content is distributed in the event-specific fields.
  */
  uchar *temp_buf;
  
  /*
    TRUE <=> this event 'owns' temp_buf and should call my_free() when done
    with it
  */
  bool event_owns_temp_buf;

  /*
    Timestamp on the master(for debugging and replication of
    NOW()/TIMESTAMP).  It is important for queries and LOAD DATA
    INFILE. This is set at the event's creation time, except for Query
    and Load (et al.) events where this is set at the query's
    execution time, which guarantees good replication (otherwise, we
    could have a query and its event with different timestamps).
  */
  my_time_t when;
  ulong     when_sec_part;
  /* The number of seconds the query took to run on the master. */
  ulong exec_time;
  /* Number of bytes written by write() function */
  size_t data_written;

  /*
    The master's server id (is preserved in the relay log; used to
    prevent from infinite loops in circular replication).
  */
  uint32 server_id;

  /**
    Some 16 flags. See the definitions above for LOG_EVENT_TIME_F,
    LOG_EVENT_FORCED_ROTATE_F, LOG_EVENT_THREAD_SPECIFIC_F,
    LOG_EVENT_SUPPRESS_USE_F, and LOG_EVENT_SKIP_REPLICATION_F for notes.
  */
  uint16 flags;

  enum_event_cache_type cache_type;

  /**
    A storage to cache the global system variable's value.
    Handling of a separate event will be governed its member.
  */
  ulong slave_exec_mode;

  Log_event_writer *writer;

#ifdef MYSQL_SERVER
  THD* thd;

  Log_event();
  Log_event(THD* thd_arg, uint16 flags_arg, bool is_transactional);

  /*
    init_show_field_list() prepares the column names and types for the
    output of SHOW BINLOG EVENTS; it is used only by SHOW BINLOG
    EVENTS.
  */
  static void init_show_field_list(THD *thd, List<Item>* field_list);
#ifdef HAVE_REPLICATION
  int net_send(Protocol *protocol, const char* log_name, my_off_t pos);

  /*
    pack_info() is used by SHOW BINLOG EVENTS; as print() it prepares and sends
    a string to display to the user, so it resembles print().
  */

  virtual void pack_info(Protocol *protocol);

#endif /* HAVE_REPLICATION */
  virtual const char* get_db()
  {
    return thd ? thd->db.str : 0;
  }
#else
  Log_event() : temp_buf(0), when(0), flags(0) {}
  ha_checksum crc;
  /* print*() functions are used by mysqlbinlog */
  virtual bool print(FILE* file, PRINT_EVENT_INFO* print_event_info) = 0;
  bool print_timestamp(IO_CACHE* file, time_t *ts = 0);
  bool print_header(IO_CACHE* file, PRINT_EVENT_INFO* print_event_info,
                    bool is_more);
  bool print_base64(IO_CACHE* file, PRINT_EVENT_INFO* print_event_info,
                    bool do_print_encoded);
#endif /* MYSQL_SERVER */

  /* The following code used for Flashback */
#ifdef MYSQL_CLIENT
  my_bool is_flashback;
  my_bool need_flashback_review;
  String  output_buf; // Storing the event output
#ifdef WHEN_FLASHBACK_REVIEW_READY
  String  m_review_dbname;
  String  m_review_tablename;

  void set_review_dbname(const char *name)
  {
    if (name)
    {
      m_review_dbname.free();
      m_review_dbname.append(name);
    }
  }
  void set_review_tablename(const char *name)
  {
    if (name)
    {
      m_review_tablename.free();
      m_review_tablename.append(name);
    }
  }
  const char *get_review_dbname() const { return m_review_dbname.ptr(); }
  const char *get_review_tablename() const { return m_review_tablename.ptr(); }
#endif
#endif

  /*
    read_log_event() functions read an event from a binlog or relay
    log; used by SHOW BINLOG EVENTS, the binlog_dump thread on the
    master (reads master's binlog), the slave IO thread (reads the
    event sent by binlog_dump), the slave SQL thread (reads the event
    from the relay log).  If mutex is 0, the read will proceed without
    mutex.  We need the description_event to be able to parse the
    event (to know the post-header's size); in fact in read_log_event
    we detect the event's type, then call the specific event's
    constructor and pass description_event as an argument.
  */
  static Log_event* read_log_event(IO_CACHE* file,
                                   const Format_description_log_event
                                   *description_event,
                                   my_bool crc_check);

  /**
    Reads an event from a binlog or relay log. Used by the dump thread
    this method reads the event into a raw buffer without parsing it.

    @Note If mutex is 0, the read will proceed without mutex.

    @Note If a log name is given than the method will check if the
    given binlog is still active.

    @param[in]  file                log file to be read
    @param[out] packet              packet to hold the event
    @param[in]  checksum_alg_arg    verify the event checksum using this
                                    algorithm (or don't if it's
                                    use BINLOG_CHECKSUM_ALG_OFF)

    @retval 0                   success
    @retval LOG_READ_EOF        end of file, nothing was read
    @retval LOG_READ_BOGUS      malformed event
    @retval LOG_READ_IO         io error while reading
    @retval LOG_READ_MEM        packet memory allocation failed
    @retval LOG_READ_TRUNC      only a partial event could be read
    @retval LOG_READ_TOO_LARGE  event too large
   */
  static int read_log_event(IO_CACHE* file, String* packet,
                            const Format_description_log_event *fdle,
                            enum enum_binlog_checksum_alg checksum_alg_arg);
  /* 
     The value is set by caller of FD constructor and
     Log_event::write_header() for the rest.
     In the FD case it's propagated into the last byte 
     of post_header_len[] at FD::write().
     On the slave side the value is assigned from post_header_len[last] 
     of the last seen FD event.
  */
  enum enum_binlog_checksum_alg checksum_alg;

  static void *operator new(size_t size)
  {
    extern PSI_memory_key key_memory_log_event;
    return my_malloc(key_memory_log_event, size, MYF(MY_WME|MY_FAE));
  }

  static void operator delete(void *ptr, size_t)
  {
    my_free(ptr);
  }

  /* Placement version of the above operators */
  static void *operator new(size_t, void* ptr) { return ptr; }
  static void operator delete(void*, void*) { }

#ifdef MYSQL_SERVER
  bool write_header(size_t event_data_length);
  bool write_data(const uchar *buf, size_t data_length)
  { return writer->write_data(buf, data_length); }
  bool write_data(const char *buf, size_t data_length)
  { return write_data((uchar*)buf, data_length); }
  bool write_footer()
  { return writer->write_footer(); }

  my_bool need_checksum();

  virtual bool write()
  {
    return write_header(get_data_size()) || write_data_header() ||
	   write_data_body() || write_footer();
  }
  virtual bool write_data_header()
  { return 0; }
  virtual bool write_data_body()
  { return 0; }

  /* Return start of query time or current time */
  inline my_time_t get_time()
  {
    THD *tmp_thd;
    if (when)
      return when;
    if (thd)
    {
      when= thd->start_time;
      when_sec_part= thd->start_time_sec_part;
      return when;
    }
    /* thd will only be 0 here at time of log creation */
    if ((tmp_thd= current_thd))
    {
      when= tmp_thd->start_time;
      when_sec_part= tmp_thd->start_time_sec_part;
      return when;
    }
    my_hrtime_t hrtime= my_hrtime();
    when= hrtime_to_my_time(hrtime);
    when_sec_part= hrtime_sec_part(hrtime);
    return when;
  }
#endif
  virtual Log_event_type get_type_code() = 0;
  virtual enum_logged_status logged_status() { return LOGGED_CRITICAL; }
  virtual bool is_valid() const = 0;
  virtual my_off_t get_header_len(my_off_t len) { return len; }
  void set_artificial_event() { flags |= LOG_EVENT_ARTIFICIAL_F; }
  void set_relay_log_event() { flags |= LOG_EVENT_RELAY_LOG_F; }
  bool is_artificial_event() const { return flags & LOG_EVENT_ARTIFICIAL_F; }
  bool is_relay_log_event() const { return flags & LOG_EVENT_RELAY_LOG_F; }
  inline bool use_trans_cache() const
  { 
    return (cache_type == Log_event::EVENT_TRANSACTIONAL_CACHE);
  }
  inline void set_direct_logging()
  {
    cache_type = Log_event::EVENT_NO_CACHE;
  }
  inline bool use_direct_logging()
  {
    return (cache_type == Log_event::EVENT_NO_CACHE);
  }
  Log_event(const uchar *buf, const Format_description_log_event
            *description_event);
  virtual ~Log_event() { free_temp_buf();}
  void register_temp_buf(uchar* buf, bool must_free)
  { 
    temp_buf= buf; 
    event_owns_temp_buf= must_free;
  }
  void free_temp_buf()
  {
    if (temp_buf)
    {
      if (event_owns_temp_buf)
        my_free(temp_buf);
      temp_buf = 0;
    }
  }
  /*
    Get event length for simple events. For complicated events the length
    is calculated during write()
  */
  virtual int get_data_size() { return 0;}
  static Log_event* read_log_event(const uchar *buf, uint event_len,
				   const char **error,
                                   const Format_description_log_event
                                   *description_event, my_bool crc_check);
  /**
    Returns the human readable name of the given event type.
  */
  static const char* get_type_str(Log_event_type type);
  /**
    Returns the human readable name of this event's type.
  */
  const char* get_type_str();

#if defined(MYSQL_SERVER) && defined(HAVE_REPLICATION)

  /**
     Apply the event to the database.

     This function represents the public interface for applying an
     event.

     @see do_apply_event
   */
  int apply_event(rpl_group_info *rgi)
  {
    int res;
    THD_STAGE_INFO(thd, stage_apply_event);
    res= do_apply_event(rgi);
    THD_STAGE_INFO(thd, stage_after_apply_event);
    return res;
  }


  /**
     Update the relay log position.

     This function represents the public interface for "stepping over"
     the event and will update the relay log information.

     @see do_update_pos
   */
  int update_pos(rpl_group_info *rgi)
  {
    return do_update_pos(rgi);
  }

  /**
     Decide if the event shall be skipped, and the reason for skipping
     it.

     @see do_shall_skip
   */
  enum_skip_reason shall_skip(rpl_group_info *rgi)
  {
    return do_shall_skip(rgi);
  }


  /*
    Check if an event is non-final part of a stand-alone event group,
    such as Intvar_log_event (such events should be processed as part
    of the following event group, not individually).
    See also is_part_of_group()
  */
  static bool is_part_of_group(enum Log_event_type ev_type)
  {
    switch (ev_type)
    {
    case GTID_EVENT:
    case INTVAR_EVENT:
    case RAND_EVENT:
    case USER_VAR_EVENT:
    case TABLE_MAP_EVENT:
    case ANNOTATE_ROWS_EVENT:
      return true;
    case DELETE_ROWS_EVENT:
    case UPDATE_ROWS_EVENT:
    case WRITE_ROWS_EVENT:
    /*
      ToDo: also check for non-final Rows_log_event (though such events
      are usually in a BEGIN-COMMIT group).
    */
    default:
      return false;
    }
  }
  /*
    Same as above, but works on the object. In addition this is true for all
    rows event except the last one.
  */
  virtual bool is_part_of_group() { return 0; }

  static bool is_group_event(enum Log_event_type ev_type)
  {
    switch (ev_type)
    {
    case START_EVENT_V3:
    case STOP_EVENT:
    case ROTATE_EVENT:
    case SLAVE_EVENT:
    case FORMAT_DESCRIPTION_EVENT:
    case INCIDENT_EVENT:
    case HEARTBEAT_LOG_EVENT:
    case BINLOG_CHECKPOINT_EVENT:
    case GTID_LIST_EVENT:
    case START_ENCRYPTION_EVENT:
      return false;

    default:
      return true;
    }
  }
  
protected:

  /**
     Helper function to ignore an event w.r.t. the slave skip counter.

     This function can be used inside do_shall_skip() for functions
     that cannot end a group. If the slave skip counter is 1 when
     seeing such an event, the event shall be ignored, the counter
     left intact, and processing continue with the next event.

     A typical usage is:
     @code
     enum_skip_reason do_shall_skip(rpl_group_info *rgi) {
       return continue_group(rgi);
     }
     @endcode

     @return Skip reason
   */
  enum_skip_reason continue_group(rpl_group_info *rgi);

  /**
    Primitive to apply an event to the database.

    This is where the change to the database is made.

    @note The primitive is protected instead of private, since there
    is a hierarchy of actions to be performed in some cases.

    @see Format_description_log_event::do_apply_event()

    @param rli Pointer to relay log info structure

    @retval 0     Event applied successfully
    @retval errno Error code if event application failed
  */
  virtual int do_apply_event(rpl_group_info *rgi)
  {
    return 0;                /* Default implementation does nothing */
  }


  /**
     Advance relay log coordinates.

     This function is called to advance the relay log coordinates to
     just after the event.  It is essential that both the relay log
     coordinate and the group log position is updated correctly, since
     this function is used also for skipping events.

     Normally, each implementation of do_update_pos() shall:

     - Update the event position to refer to the position just after
       the event.

     - Update the group log position to refer to the position just
       after the event <em>if the event is last in a group</em>

     @param rli Pointer to relay log info structure

     @retval 0     Coordinates changed successfully
     @retval errno Error code if advancing failed (usually just
                   1). Observe that handler errors are returned by the
                   do_apply_event() function, and not by this one.
   */
  virtual int do_update_pos(rpl_group_info *rgi);


  /**
     Decide if this event shall be skipped or not and the reason for
     skipping it.

     The default implementation decide that the event shall be skipped
     if either:

     - the server id of the event is the same as the server id of the
       server and <code>rli->replicate_same_server_id</code> is true,
       or

     - if <code>rli->slave_skip_counter</code> is greater than zero.

     @see do_apply_event
     @see do_update_pos

     @retval Log_event::EVENT_SKIP_NOT
     The event shall not be skipped and should be applied.

     @retval Log_event::EVENT_SKIP_IGNORE
     The event shall be skipped by just ignoring it, i.e., the slave
     skip counter shall not be changed. This happends if, for example,
     the originating server id of the event is the same as the server
     id of the slave.

     @retval Log_event::EVENT_SKIP_COUNT
     The event shall be skipped because the slave skip counter was
     non-zero. The caller shall decrease the counter by one.
   */
  virtual enum_skip_reason do_shall_skip(rpl_group_info *rgi);
#endif
};


/*
   One class for each type of event.
   Two constructors for each class:
   - one to create the event for logging (when the server acts as a master),
   called after an update to the database is done,
   which accepts parameters like the query, the database, the options for LOAD
   DATA INFILE...
   - one to create the event from a packet (when the server acts as a slave),
   called before reproducing the update, which accepts parameters (like a
   buffer). Used to read from the master, from the relay log, and in
   mysqlbinlog. This constructor must be format-tolerant.
*/

/**
  @class Query_log_event
   
  A @c Query_log_event is created for each query that modifies the
  database, unless the query is logged row-based.

  @section Query_log_event_binary_format Binary format

  See @ref Log_event_binary_format "Binary format for log events" for
  a general discussion and introduction to the binary format of binlog
  events.

  The Post-Header has five components:

  <table>
  <caption>Post-Header for Query_log_event</caption>

  <tr>
    <th>Name</th>
    <th>Format</th>
    <th>Description</th>
  </tr>

  <tr>
    <td>slave_proxy_id</td>
    <td>4 byte unsigned integer</td>
    <td>An integer identifying the client thread that issued the
    query.  The id is unique per server.  (Note, however, that two
    threads on different servers may have the same slave_proxy_id.)
    This is used when a client thread creates a temporary table local
    to the client.  The slave_proxy_id is used to distinguish
    temporary tables that belong to different clients.
    </td>
  </tr>

  <tr>
    <td>exec_time</td>
    <td>4 byte unsigned integer</td>
    <td>The time from when the query started to when it was logged in
    the binlog, in seconds.</td>
  </tr>

  <tr>
    <td>db_len</td>
    <td>1 byte integer</td>
    <td>The length of the name of the currently selected database.</td>
  </tr>

  <tr>
    <td>error_code</td>
    <td>2 byte unsigned integer</td>
    <td>Error code generated by the master.  If the master fails, the
    slave will fail with the same error code, except for the error
    codes ER_DB_CREATE_EXISTS == 1007 and ER_DB_DROP_EXISTS == 1008.
    </td>
  </tr>

  <tr>
    <td>status_vars_len</td>
    <td>2 byte unsigned integer</td>
    <td>The length of the status_vars block of the Body, in bytes. See
    @ref query_log_event_status_vars "below".
    </td>
  </tr>
  </table>

  The Body has the following components:

  <table>
  <caption>Body for Query_log_event</caption>

  <tr>
    <th>Name</th>
    <th>Format</th>
    <th>Description</th>
  </tr>

  <tr>
    <td>@anchor query_log_event_status_vars status_vars</td>
    <td>status_vars_len bytes</td>
    <td>Zero or more status variables.  Each status variable consists
    of one byte identifying the variable stored, followed by the value
    of the variable.  The possible variables are listed separately in
    the table @ref Table_query_log_event_status_vars "below".  MySQL
    always writes events in the order defined below; however, it is
    capable of reading them in any order.  </td>
  </tr>

  <tr>
    <td>db</td>
    <td>db_len+1</td>
    <td>The currently selected database, as a null-terminated string.

    (The trailing zero is redundant since the length is already known;
    it is db_len from Post-Header.)
    </td>
  </tr>

  <tr>
    <td>query</td>
    <td>variable length string without trailing zero, extending to the
    end of the event (determined by the length field of the
    Common-Header)
    </td>
    <td>The SQL query.</td>
  </tr>
  </table>

  The following table lists the status variables that may appear in
  the status_vars field.

  @anchor Table_query_log_event_status_vars
  <table>
  <caption>Status variables for Query_log_event</caption>

  <tr>
    <th>Status variable</th>
    <th>1 byte identifier</th>
    <th>Format</th>
    <th>Description</th>
  </tr>

  <tr>
    <td>flags2</td>
    <td>Q_FLAGS2_CODE == 0</td>
    <td>4 byte bitfield</td>
    <td>The flags in @c thd->options, binary AND-ed with @c
    OPTIONS_WRITTEN_TO_BIN_LOG.  The @c thd->options bitfield contains
    options for "SELECT".  @c OPTIONS_WRITTEN identifies those options
    that need to be written to the binlog (not all do).  Specifically,
    @c OPTIONS_WRITTEN_TO_BIN_LOG equals (@c OPTION_AUTO_IS_NULL | @c
    OPTION_NO_FOREIGN_KEY_CHECKS | @c OPTION_RELAXED_UNIQUE_CHECKS |
    @c OPTION_NOT_AUTOCOMMIT), or 0x0c084000 in hex.

    These flags correspond to the SQL variables SQL_AUTO_IS_NULL,
    FOREIGN_KEY_CHECKS, UNIQUE_CHECKS, and AUTOCOMMIT, documented in
    the "SET Syntax" section of the MySQL Manual.

    This field is always written to the binlog in version >= 5.0, and
    never written in version < 5.0.
    </td>
  </tr>

  <tr>
    <td>sql_mode</td>
    <td>Q_SQL_MODE_CODE == 1</td>
    <td>8 byte bitfield</td>
    <td>The @c sql_mode variable.  See the section "SQL Modes" in the
    MySQL manual, and see sql_priv.h for a list of the possible
    flags. Currently (2007-10-04), the following flags are available:
    <pre>
    MODE_REAL_AS_FLOAT==0x1
    MODE_PIPES_AS_CONCAT==0x2
    MODE_ANSI_QUOTES==0x4
    MODE_IGNORE_SPACE==0x8
    MODE_IGNORE_BAD_TABLE_OPTIONS==0x10
    MODE_ONLY_FULL_GROUP_BY==0x20
    MODE_NO_UNSIGNED_SUBTRACTION==0x40
    MODE_NO_DIR_IN_CREATE==0x80
    MODE_POSTGRESQL==0x100
    MODE_ORACLE==0x200
    MODE_MSSQL==0x400
    MODE_DB2==0x800
    MODE_MAXDB==0x1000
    MODE_NO_KEY_OPTIONS==0x2000
    MODE_NO_TABLE_OPTIONS==0x4000
    MODE_NO_FIELD_OPTIONS==0x8000
    MODE_MYSQL323==0x10000
    MODE_MYSQL323==0x20000
    MODE_MYSQL40==0x40000
    MODE_ANSI==0x80000
    MODE_NO_AUTO_VALUE_ON_ZERO==0x100000
    MODE_NO_BACKSLASH_ESCAPES==0x200000
    MODE_STRICT_TRANS_TABLES==0x400000
    MODE_STRICT_ALL_TABLES==0x800000
    MODE_NO_ZERO_IN_DATE==0x1000000
    MODE_NO_ZERO_DATE==0x2000000
    MODE_INVALID_DATES==0x4000000
    MODE_ERROR_FOR_DIVISION_BY_ZERO==0x8000000
    MODE_TRADITIONAL==0x10000000
    MODE_NO_AUTO_CREATE_USER==0x20000000
    MODE_HIGH_NOT_PRECEDENCE==0x40000000
    MODE_PAD_CHAR_TO_FULL_LENGTH==0x80000000
    </pre>
    All these flags are replicated from the server.  However, all
    flags except @c MODE_NO_DIR_IN_CREATE are honored by the slave;
    the slave always preserves its old value of @c
    MODE_NO_DIR_IN_CREATE.  For a rationale, see comment in
    @c Query_log_event::do_apply_event in @c log_event.cc.

    This field is always written to the binlog.
    </td>
  </tr>

  <tr>
    <td>catalog</td>
    <td>Q_CATALOG_NZ_CODE == 6</td>
    <td>Variable-length string: the length in bytes (1 byte) followed
    by the characters (at most 255 bytes)
    </td>
    <td>Stores the client's current catalog.  Every database belongs
    to a catalog, the same way that every table belongs to a
    database.  Currently, there is only one catalog, "std".

    This field is written if the length of the catalog is > 0;
    otherwise it is not written.
    </td>
  </tr>

  <tr>
    <td>auto_increment</td>
    <td>Q_AUTO_INCREMENT == 3</td>
    <td>two 2 byte unsigned integers, totally 2+2=4 bytes</td>

    <td>The two variables auto_increment_increment and
    auto_increment_offset, in that order.  For more information, see
    "System variables" in the MySQL manual.

    This field is written if auto_increment > 1.  Otherwise, it is not
    written.
    </td>
  </tr>

  <tr>
    <td>charset</td>
    <td>Q_CHARSET_CODE == 4</td>
    <td>three 2 byte unsigned integers, totally 2+2+2=6 bytes</td>
    <td>The three variables character_set_client,
    collation_connection, and collation_server, in that order.
    character_set_client is a code identifying the character set and
    collation used by the client to encode the query.
    collation_connection identifies the character set and collation
    that the master converts the query to when it receives it; this is
    useful when comparing literal strings.  collation_server is the
    default character set and collation used when a new database is
    created.

    See also "Connection Character Sets and Collations" in the MySQL
    5.1 manual.

    All three variables are codes identifying a (character set,
    collation) pair.  To see which codes map to which pairs, run the
    query "SELECT id, character_set_name, collation_name FROM
    COLLATIONS".

    Cf. Q_CHARSET_DATABASE_CODE below.

    This field is always written.
    </td>
  </tr>

  <tr>
    <td>time_zone</td>
    <td>Q_TIME_ZONE_CODE == 5</td>
    <td>Variable-length string: the length in bytes (1 byte) followed
    by the characters (at most 255 bytes).
    <td>The time_zone of the master.

    See also "System Variables" and "MySQL Server Time Zone Support"
    in the MySQL manual.

    This field is written if the length of the time zone string is >
    0; otherwise, it is not written.
    </td>
  </tr>

  <tr>
    <td>lc_time_names_number</td>
    <td>Q_LC_TIME_NAMES_CODE == 7</td>
    <td>2 byte integer</td>
    <td>A code identifying a table of month and day names.  The
    mapping from codes to languages is defined in @c sql_locale.cc.

    This field is written if it is not 0, i.e., if the locale is not
    en_US.
    </td>
  </tr>

  <tr>
    <td>charset_database_number</td>
    <td>Q_CHARSET_DATABASE_CODE == 8</td>
    <td>2 byte integer</td>

    <td>The value of the collation_database system variable (in the
    source code stored in @c thd->variables.collation_database), which
    holds the code for a (character set, collation) pair as described
    above (see Q_CHARSET_CODE).

    collation_database was used in old versions (???WHEN).  Its value
    was loaded when issuing a "use db" query and could be changed by
    issuing a "SET collation_database=xxx" query.  It used to affect
    the "LOAD DATA INFILE" and "CREATE TABLE" commands.

    In newer versions, "CREATE TABLE" has been changed to take the
    character set from the database of the created table, rather than
    the character set of the current database.  This makes a
    difference when creating a table in another database than the
    current one.  "LOAD DATA INFILE" has not yet changed to do this,
    but there are plans to eventually do it, and to make
    collation_database read-only.

    This field is written if it is not 0.
    </td>
  </tr>
  <tr>
    <td>table_map_for_update</td>
    <td>Q_TABLE_MAP_FOR_UPDATE_CODE == 9</td>
    <td>8 byte integer</td>

    <td>The value of the table map that is to be updated by the
    multi-table update query statement. Every bit of this variable
    represents a table, and is set to 1 if the corresponding table is
    to be updated by this statement.

    The value of this variable is set when executing a multi-table update
    statement and used by slave to apply filter rules without opening
    all the tables on slave. This is required because some tables may
    not exist on slave because of the filter rules.
    </td>
  </tr>
  </table>

  @subsection Query_log_event_notes_on_previous_versions Notes on Previous Versions

  * Status vars were introduced in version 5.0.  To read earlier
  versions correctly, check the length of the Post-Header.

  * The status variable Q_CATALOG_CODE == 2 existed in MySQL 5.0.x,
  where 0<=x<=3.  It was identical to Q_CATALOG_CODE, except that the
  string had a trailing '\0'.  The '\0' was removed in 5.0.4 since it
  was redundant (the string length is stored before the string).  The
  Q_CATALOG_CODE will never be written by a new master, but can still
  be understood by a new slave.

  * See Q_CHARSET_DATABASE_CODE in the table above.

  * When adding new status vars, please don't forget to update the
  MAX_SIZE_LOG_EVENT_STATUS, and update function code_name

*/
class Query_log_event: public Log_event
{
  LEX_CSTRING user;
  LEX_CSTRING host;
protected:
  Log_event::Byte* data_buf;
public:
  const char* query;
  const char* catalog;
  const char* db;
  /*
    If we already know the length of the query string
    we pass it with q_len, so we would not have to call strlen()
    otherwise, set it to 0, in which case, we compute it with strlen()
  */
  uint32 q_len;
  uint32 db_len;
  uint16 error_code;
  my_thread_id thread_id;
  /*
    For events created by Query_log_event::do_apply_event (and
    Load_log_event::do_apply_event()) we need the *original* thread
    id, to be able to log the event with the original (=master's)
    thread id (fix for BUG#1686).
  */
  ulong slave_proxy_id;

  /*
    Binlog format 3 and 4 start to differ (as far as class members are
    concerned) from here.
  */

  uint catalog_len;			// <= 255 char; 0 means uninited

  /*
    We want to be able to store a variable number of N-bit status vars:
    (generally N=32; but N=64 for SQL_MODE) a user may want to log the number
    of affected rows (for debugging) while another does not want to lose 4
    bytes in this.
    The storage on disk is the following:
    status_vars_len is part of the post-header,
    status_vars are in the variable-length part, after the post-header, before
    the db & query.
    status_vars on disk is a sequence of pairs (code, value) where 'code' means
    'sql_mode', 'affected' etc. Sometimes 'value' must be a short string, so
    its first byte is its length. For now the order of status vars is:
    flags2 - sql_mode - catalog - autoinc - charset
    We should add the same thing to Load_log_event, but in fact
    LOAD DATA INFILE is going to be logged with a new type of event (logging of
    the plain text query), so Load_log_event would be frozen, so no need. The
    new way of logging LOAD DATA INFILE would use a derived class of
    Query_log_event, so automatically benefit from the work already done for
    status variables in Query_log_event.
 */
  uint16 status_vars_len;

  /*
    'flags2' is a second set of flags (on top of those in Log_event), for
    session variables. These are thd->options which is & against a mask
    (OPTIONS_WRITTEN_TO_BIN_LOG).
    flags2_inited helps make a difference between flags2==0 (3.23 or 4.x
    master, we don't know flags2, so use the slave server's global options) and
    flags2==0 (5.0 master, we know this has a meaning of flags all down which
    must influence the query).
  */
  bool flags2_inited;
  bool sql_mode_inited;
  bool charset_inited;

  uint32 flags2;
  sql_mode_t sql_mode;
  ulong auto_increment_increment, auto_increment_offset;
  char charset[6];
  uint time_zone_len; /* 0 means uninited */
  const char *time_zone_str;
  uint lc_time_names_number; /* 0 means en_US */
  uint charset_database_number;
  /*
    map for tables that will be updated for a multi-table update query
    statement, for other query statements, this will be zero.
  */
  ulonglong table_map_for_update;
  /* Xid for the event, if such exists */
  ulonglong xid;
  /*
    Holds the original length of a Query_log_event that comes from a
    master of version < 5.0 (i.e., binlog_version < 4). When the IO
    thread writes the relay log, it augments the Query_log_event with a
    Q_MASTER_DATA_WRITTEN_CODE status_var that holds the original event
    length. This field is initialized to non-zero in the SQL thread when
    it reads this augmented event. SQL thread does not write 
    Q_MASTER_DATA_WRITTEN_CODE to the slave's server binlog.
  */
  uint32 master_data_written;

#ifdef MYSQL_SERVER

  Query_log_event(THD* thd_arg, const char* query_arg, size_t query_length,
                  bool using_trans, bool direct, bool suppress_use, int error);
  const char* get_db() { return db; }
#ifdef HAVE_REPLICATION
  void pack_info(Protocol* protocol);
#endif /* HAVE_REPLICATION */
#else
  bool print_query_header(IO_CACHE* file, PRINT_EVENT_INFO* print_event_info);
  bool print(FILE* file, PRINT_EVENT_INFO* print_event_info);
#endif

  Query_log_event();
  Query_log_event(const uchar *buf, uint event_len,
                  const Format_description_log_event *description_event,
                  Log_event_type event_type);
  ~Query_log_event()
  {
    if (data_buf)
      my_free(data_buf);
  }
  Log_event_type get_type_code() { return QUERY_EVENT; }
  static int dummy_event(String *packet, ulong ev_offset, enum enum_binlog_checksum_alg checksum_alg);
  static int begin_event(String *packet, ulong ev_offset, enum enum_binlog_checksum_alg checksum_alg);
#ifdef MYSQL_SERVER
  bool write();
  virtual bool write_post_header_for_derived() { return FALSE; }
#endif
  bool is_valid() const { return query != 0; }

  /*
    Returns number of bytes additionally written to post header by derived
    events (so far it is only Execute_load_query event).
  */
  virtual ulong get_post_header_size_for_derived() { return 0; }
  /* Writes derived event-specific part of post header. */

public:        /* !!! Public in this patch to allow old usage */
#if defined(MYSQL_SERVER) && defined(HAVE_REPLICATION)
  virtual enum_skip_reason do_shall_skip(rpl_group_info *rgi);
  virtual int do_apply_event(rpl_group_info *rgi);

  int do_apply_event(rpl_group_info *rgi,
                       const char *query_arg,
                       uint32 q_len_arg);
  static bool peek_is_commit_rollback(const uchar *event_start,
                                      size_t event_len,
                                      enum enum_binlog_checksum_alg
                                      checksum_alg);
#endif /* HAVE_REPLICATION */
  /*
    If true, the event always be applied by slave SQL thread or be printed by
    mysqlbinlog
   */
  bool is_trans_keyword()
  {
    /*
      Before the patch for bug#50407, The 'SAVEPOINT and ROLLBACK TO'
      queries input by user was written into log events directly.
      So the keywords can be written in both upper case and lower case
      together, strncasecmp is used to check both cases. they also could be
      binlogged with comments in the front of these keywords. for examples:
        / * bla bla * / SAVEPOINT a;
        / * bla bla * / ROLLBACK TO a;
      but we don't handle these cases and after the patch, both quiries are
      binlogged in upper case with no comments.
     */
    return !strncmp(query, "BEGIN", q_len) ||
      !strncmp(query, "COMMIT", q_len) ||
      !strncasecmp(query, "SAVEPOINT", 9) ||
      !strncasecmp(query, "ROLLBACK", 8);
  }
  virtual bool is_begin()    { return !strcmp(query, "BEGIN"); }
  virtual bool is_commit()   { return !strcmp(query, "COMMIT"); }
  virtual bool is_rollback() { return !strcmp(query, "ROLLBACK"); }
};

class Query_compressed_log_event:public Query_log_event{
protected:
  Log_event::Byte* query_buf;  // point to the uncompressed query
public:
  Query_compressed_log_event(const uchar *buf, uint event_len,
    const Format_description_log_event *description_event,
    Log_event_type event_type);
  ~Query_compressed_log_event()
  {
    if (query_buf)
      my_free(query_buf);
  }
  Log_event_type get_type_code() { return QUERY_COMPRESSED_EVENT; }

  /*
    the min length of log_bin_compress_min_len is 10,
    means that Begin/Commit/Rollback would never be compressed!  
  */
  virtual bool is_begin()    { return false; }
  virtual bool is_commit()   { return false; }
  virtual bool is_rollback() { return false; }
#ifdef MYSQL_SERVER
  Query_compressed_log_event(THD* thd_arg, const char* query_arg,
                             ulong query_length,
                             bool using_trans, bool direct, bool suppress_use,
                             int error);
  virtual bool write();
#endif
};


/*****************************************************************************
  sql_ex_info struct
 ****************************************************************************/
struct sql_ex_info
{
  const char* field_term;
  const char* enclosed;
  const char* line_term;
  const char* line_start;
  const char* escaped;
  int cached_new_format= -1;
  uint8 field_term_len= 0, enclosed_len= 0, line_term_len= 0,
    line_start_len= 0, escaped_len= 0;
  char opt_flags;
  char empty_flags= 0;

  // store in new format even if old is possible
  void force_new_format() { cached_new_format = 1;}
  int data_size()
  {
    return (new_format() ?
	    field_term_len + enclosed_len + line_term_len +
	    line_start_len + escaped_len + 6 : 7);
  }
  bool write_data(Log_event_writer *writer);
  const uchar *init(const uchar *buf, const uchar* buf_end, bool use_new_format);
  bool new_format()
  {
    return ((cached_new_format != -1) ? cached_new_format :
	    (cached_new_format=(field_term_len > 1 ||
				enclosed_len > 1 ||
				line_term_len > 1 || line_start_len > 1 ||
				escaped_len > 1)));
  }
};

/**
  @class Load_log_event

  This log event corresponds to a "LOAD DATA INFILE" SQL query on the
  following form:

  @verbatim
   (1)    USE db;
   (2)    LOAD DATA [CONCURRENT] [LOCAL] INFILE 'file_name'
   (3)    [REPLACE | IGNORE]
   (4)    INTO TABLE 'table_name'
   (5)    [FIELDS
   (6)      [TERMINATED BY 'field_term']
   (7)      [[OPTIONALLY] ENCLOSED BY 'enclosed']
   (8)      [ESCAPED BY 'escaped']
   (9)    ]
  (10)    [LINES
  (11)      [TERMINATED BY 'line_term']
  (12)      [LINES STARTING BY 'line_start']
  (13)    ]
  (14)    [IGNORE skip_lines LINES]
  (15)    (field_1, field_2, ..., field_n)@endverbatim

  @section Load_log_event_binary_format Binary Format

  The Post-Header consists of the following six components.

  <table>
  <caption>Post-Header for Load_log_event</caption>

  <tr>
    <th>Name</th>
    <th>Format</th>
    <th>Description</th>
  </tr>

  <tr>
    <td>slave_proxy_id</td>
    <td>4 byte unsigned integer</td>
    <td>An integer identifying the client thread that issued the
    query.  The id is unique per server.  (Note, however, that two
    threads on different servers may have the same slave_proxy_id.)
    This is used when a client thread creates a temporary table local
    to the client.  The slave_proxy_id is used to distinguish
    temporary tables that belong to different clients.
    </td>
  </tr>

  <tr>
    <td>exec_time</td>
    <td>4 byte unsigned integer</td>
    <td>The time from when the query started to when it was logged in
    the binlog, in seconds.</td>
  </tr>

  <tr>
    <td>skip_lines</td>
    <td>4 byte unsigned integer</td>
    <td>The number on line (14) above, if present, or 0 if line (14)
    is left out.
    </td>
  </tr>

  <tr>
    <td>table_name_len</td>
    <td>1 byte unsigned integer</td>
    <td>The length of 'table_name' on line (4) above.</td>
  </tr>

  <tr>
    <td>db_len</td>
    <td>1 byte unsigned integer</td>
    <td>The length of 'db' on line (1) above.</td>
  </tr>

  <tr>
    <td>num_fields</td>
    <td>4 byte unsigned integer</td>
    <td>The number n of fields on line (15) above.</td>
  </tr>
  </table>    

  The Body contains the following components.

  <table>
  <caption>Body of Load_log_event</caption>

  <tr>
    <th>Name</th>
    <th>Format</th>
    <th>Description</th>
  </tr>

  <tr>
    <td>sql_ex</td>
    <td>variable length</td>

    <td>Describes the part of the query on lines (3) and
    (5)&ndash;(13) above.  More precisely, it stores the five strings
    (on lines) field_term (6), enclosed (7), escaped (8), line_term
    (11), and line_start (12); as well as a bitfield indicating the
    presence of the keywords REPLACE (3), IGNORE (3), and OPTIONALLY
    (7).

    The data is stored in one of two formats, called "old" and "new".
    The type field of Common-Header determines which of these two
    formats is used: type LOAD_EVENT means that the old format is
    used, and type NEW_LOAD_EVENT means that the new format is used.
    When MySQL writes a Load_log_event, it uses the new format if at
    least one of the five strings is two or more bytes long.
    Otherwise (i.e., if all strings are 0 or 1 bytes long), the old
    format is used.

    The new and old format differ in the way the five strings are
    stored.

    <ul>
    <li> In the new format, the strings are stored in the order
    field_term, enclosed, escaped, line_term, line_start. Each string
    consists of a length (1 byte), followed by a sequence of
    characters (0-255 bytes).  Finally, a boolean combination of the
    following flags is stored in 1 byte: REPLACE_FLAG==0x4,
    IGNORE_FLAG==0x8, and OPT_ENCLOSED_FLAG==0x2.  If a flag is set,
    it indicates the presence of the corresponding keyword in the SQL
    query.

    <li> In the old format, we know that each string has length 0 or
    1.  Therefore, only the first byte of each string is stored.  The
    order of the strings is the same as in the new format.  These five
    bytes are followed by the same 1 byte bitfield as in the new
    format.  Finally, a 1 byte bitfield called empty_flags is stored.
    The low 5 bits of empty_flags indicate which of the five strings
    have length 0.  For each of the following flags that is set, the
    corresponding string has length 0; for the flags that are not set,
    the string has length 1: FIELD_TERM_EMPTY==0x1,
    ENCLOSED_EMPTY==0x2, LINE_TERM_EMPTY==0x4, LINE_START_EMPTY==0x8,
    ESCAPED_EMPTY==0x10.
    </ul>

    Thus, the size of the new format is 6 bytes + the sum of the sizes
    of the five strings.  The size of the old format is always 7
    bytes.
    </td>
  </tr>

  <tr>
    <td>field_lens</td>
    <td>num_fields 1 byte unsigned integers</td>
    <td>An array of num_fields integers representing the length of
    each field in the query.  (num_fields is from the Post-Header).
    </td>
  </tr>

  <tr>
    <td>fields</td>
    <td>num_fields null-terminated strings</td>
    <td>An array of num_fields null-terminated strings, each
    representing a field in the query.  (The trailing zero is
    redundant, since the length are stored in the num_fields array.)
    The total length of all strings equals to the sum of all
    field_lens, plus num_fields bytes for all the trailing zeros.
    </td>
  </tr>

  <tr>
    <td>table_name</td>
    <td>null-terminated string of length table_len+1 bytes</td>
    <td>The 'table_name' from the query, as a null-terminated string.
    (The trailing zero is actually redundant since the table_len is
    known from Post-Header.)
    </td>
  </tr>

  <tr>
    <td>db</td>
    <td>null-terminated string of length db_len+1 bytes</td>
    <td>The 'db' from the query, as a null-terminated string.
    (The trailing zero is actually redundant since the db_len is known
    from Post-Header.)
    </td>
  </tr>

  <tr>
    <td>file_name</td>
    <td>variable length string without trailing zero, extending to the
    end of the event (determined by the length field of the
    Common-Header)
    </td>
    <td>The 'file_name' from the query.
    </td>
  </tr>

  </table>

  @subsection Load_log_event_notes_on_previous_versions Notes on Previous Versions

  This event type is understood by current versions, but only
  generated by MySQL 3.23 and earlier.
*/
class Load_log_event: public Log_event
{
private:
protected:
  int copy_log_event(const uchar *buf, ulong event_len,
                     int body_offset,
                     const Format_description_log_event* description_event);

public:
  bool print_query(THD *thd, bool need_db, const char *cs, String *buf,
                   my_off_t *fn_start, my_off_t *fn_end,
                   const char *qualify_db);
  my_thread_id thread_id;
  ulong slave_proxy_id;
  uint32 table_name_len;
  /*
    No need to have a catalog, as these events can only come from 4.x.
    TODO: this may become false if Dmitri pushes his new LOAD DATA INFILE in
    5.0 only (not in 4.x).
  */
  uint32 db_len;
  uint32 fname_len;
  uint32 num_fields;
  const char* fields;
  const uchar* field_lens;
  uint32 field_block_len;

  const char* table_name;
  const char* db;
  const char* fname;
  uint32 skip_lines;
  sql_ex_info sql_ex;
  bool local_fname;
  /**
    Indicates that this event corresponds to LOAD DATA CONCURRENT,

    @note Since Load_log_event event coming from the binary log
          lacks information whether LOAD DATA on master was concurrent
          or not, this flag is only set to TRUE for an auxiliary
          Load_log_event object which is used in mysql_load() to
          re-construct LOAD DATA statement from function parameters,
          for logging.
  */
  bool is_concurrent;

  /* fname doesn't point to memory inside Log_event::temp_buf  */
  void set_fname_outside_temp_buf(const char *afname, size_t alen)
  {
    fname= afname;
    fname_len= (uint)alen;
    local_fname= TRUE;
  }
  /* fname doesn't point to memory inside Log_event::temp_buf  */
  int  check_fname_outside_temp_buf()
  {
    return local_fname;
  }

#ifdef MYSQL_SERVER
  String field_lens_buf;
  String fields_buf;

  Load_log_event(THD* thd, const sql_exchange* ex, const char* db_arg,
		 const char* table_name_arg,
		 List<Item>& fields_arg,
                 bool is_concurrent_arg,
                 enum enum_duplicates handle_dup, bool ignore,
		 bool using_trans);
  void set_fields(const char* db, List<Item> &fields_arg,
                  Name_resolution_context *context);
  const char* get_db() { return db; }
#ifdef HAVE_REPLICATION
  void pack_info(Protocol* protocol);
#endif /* HAVE_REPLICATION */
#else
  bool print(FILE* file, PRINT_EVENT_INFO* print_event_info);
  bool print(FILE* file, PRINT_EVENT_INFO* print_event_info, bool commented);
#endif

  /*
    Note that for all the events related to LOAD DATA (Load_log_event,
    Create_file/Append/Exec/Delete, we pass description_event; however as
    logging of LOAD DATA is going to be changed in 4.1 or 5.0, this is only used
    for the common_header_len (post_header_len will not be changed).
  */
  Load_log_event(const uchar *buf, uint event_len,
                 const Format_description_log_event* description_event);
  ~Load_log_event()
  {}
  Log_event_type get_type_code()
  {
    return sql_ex.new_format() ? NEW_LOAD_EVENT: LOAD_EVENT;
  }
#ifdef MYSQL_SERVER
  bool write_data_header();
  bool write_data_body();
#endif
  bool is_valid() const { return table_name != 0; }
  int get_data_size()
  {
    return (table_name_len + db_len + 2 + fname_len
	    + LOAD_HEADER_LEN
	    + sql_ex.data_size() + field_block_len + num_fields);
  }

public:        /* !!! Public in this patch to allow old usage */
#if defined(MYSQL_SERVER) && defined(HAVE_REPLICATION)
  virtual int do_apply_event(rpl_group_info *rgi)
  {
    return do_apply_event(thd->slave_net,rgi,0);
  }

  int do_apply_event(NET *net, rpl_group_info *rgi,
                     bool use_rli_only_for_errors);
#endif
};

/**
  @class Start_log_event_v3

  Start_log_event_v3 is the Start_log_event of binlog format 3 (MySQL 3.23 and
  4.x).

  Format_description_log_event derives from Start_log_event_v3; it is
  the Start_log_event of binlog format 4 (MySQL 5.0), that is, the
  event that describes the other events' Common-Header/Post-Header
  lengths. This event is sent by MySQL 5.0 whenever it starts sending
  a new binlog if the requested position is >4 (otherwise if ==4 the
  event will be sent naturally).

  @section Start_log_event_v3_binary_format Binary Format
*/
class Start_log_event_v3: public Log_event
{
public:
  /*
    If this event is at the start of the first binary log since server
    startup 'created' should be the timestamp when the event (and the
    binary log) was created.  In the other case (i.e. this event is at
    the start of a binary log created by FLUSH LOGS or automatic
    rotation), 'created' should be 0.  This "trick" is used by MySQL
    >=4.0.14 slaves to know whether they must drop stale temporary
    tables and whether they should abort unfinished transaction.

    Note that when 'created'!=0, it is always equal to the event's
    timestamp; indeed Start_log_event is written only in log.cc where
    the first constructor below is called, in which 'created' is set
    to 'when'.  So in fact 'created' is a useless variable. When it is
    0 we can read the actual value from timestamp ('when') and when it
    is non-zero we can read the same value from timestamp
    ('when'). Conclusion:
     - we use timestamp to print when the binlog was created.
     - we use 'created' only to know if this is a first binlog or not.
     In 3.23.57 we did not pay attention to this identity, so mysqlbinlog in
     3.23.57 does not print 'created the_date' if created was zero. This is now
     fixed.
  */
  time_t created;
  uint16 binlog_version;
  char server_version[ST_SERVER_VER_LEN];
  /*
    We set this to 1 if we don't want to have the created time in the log,
    which is the case when we rollover to a new log.
  */
  bool dont_set_created;

#ifdef MYSQL_SERVER
  Start_log_event_v3();
#ifdef HAVE_REPLICATION
  void pack_info(Protocol* protocol);
#endif /* HAVE_REPLICATION */
#else
  Start_log_event_v3() {}
  bool print(FILE* file, PRINT_EVENT_INFO* print_event_info);
#endif

  Start_log_event_v3(const uchar *buf, uint event_len,
                     const Format_description_log_event* description_event);
  ~Start_log_event_v3() {}
  Log_event_type get_type_code() { return START_EVENT_V3;}
  my_off_t get_header_len(my_off_t l __attribute__((unused)))
  { return LOG_EVENT_MINIMAL_HEADER_LEN; }
#ifdef MYSQL_SERVER
  bool write();
#endif
  bool is_valid() const { return server_version[0] != 0; }
  int get_data_size()
  {
    return START_V3_HEADER_LEN; //no variable-sized part
  }

protected:
#if defined(MYSQL_SERVER) && defined(HAVE_REPLICATION)
  virtual int do_apply_event(rpl_group_info *rgi);
  virtual enum_skip_reason do_shall_skip(rpl_group_info*)
  {
    /*
      Events from ourself should be skipped, but they should not
      decrease the slave skip counter.
     */
    if (this->server_id == global_system_variables.server_id)
      return Log_event::EVENT_SKIP_IGNORE;
    else
      return Log_event::EVENT_SKIP_NOT;
  }
#endif
};

/**
  @class Start_encryption_log_event

  Start_encryption_log_event marks the beginning of encrypted data (all events
  after this event are encrypted).

  It contains the cryptographic scheme used for the encryption as well as any
  data required to decrypt (except the actual key).

  For binlog cryptoscheme 1: key version, and nonce for iv generation.
*/
class Start_encryption_log_event : public Log_event
{
public:
#ifdef MYSQL_SERVER
  Start_encryption_log_event(uint crypto_scheme_arg, uint key_version_arg,
                             const uchar* nonce_arg)
  : crypto_scheme(crypto_scheme_arg), key_version(key_version_arg)
  {
    cache_type = EVENT_NO_CACHE;
    DBUG_ASSERT(crypto_scheme == 1);
    memcpy(nonce, nonce_arg, BINLOG_NONCE_LENGTH);
  }

  bool write_data_body()
  {
    uchar scheme_buf= crypto_scheme;
    uchar key_version_buf[BINLOG_KEY_VERSION_LENGTH];
    int4store(key_version_buf, key_version);
    return write_data(&scheme_buf, sizeof(scheme_buf)) ||
           write_data(key_version_buf, sizeof(key_version_buf)) ||
           write_data(nonce, BINLOG_NONCE_LENGTH);
  }
#else
  bool print(FILE* file, PRINT_EVENT_INFO* print_event_info);
#endif

  Start_encryption_log_event(const uchar *buf, uint event_len,
                             const Format_description_log_event
                             *description_event);

  bool is_valid() const { return crypto_scheme == 1; }

  Log_event_type get_type_code() { return START_ENCRYPTION_EVENT; }

  int get_data_size()
  {
    return BINLOG_CRYPTO_SCHEME_LENGTH + BINLOG_KEY_VERSION_LENGTH +
           BINLOG_NONCE_LENGTH;
  }

  uint crypto_scheme;
  uint key_version;
  uchar nonce[BINLOG_NONCE_LENGTH];

protected:
#if defined(MYSQL_SERVER) && defined(HAVE_REPLICATION)
  virtual int do_apply_event(rpl_group_info* rgi);
  virtual int do_update_pos(rpl_group_info *rgi);
  virtual enum_skip_reason do_shall_skip(rpl_group_info* rgi)
  {
     return Log_event::EVENT_SKIP_NOT;
  }
#endif

};


class Version
{
protected:
  uchar m_ver[3];
  int cmp(const Version &other) const
  {
    return memcmp(m_ver, other.m_ver, 3);
  }
public:
  Version()
  {
    m_ver[0]= m_ver[1]= m_ver[2]= '\0';
  }
  Version(uchar v0, uchar v1, uchar v2)
  {
    m_ver[0]= v0;
    m_ver[1]= v1;
    m_ver[2]= v2;
  }
  Version(const char *version, const char **endptr);
  const uchar& operator [] (size_t i) const
  {
    DBUG_ASSERT(i < 3);
    return m_ver[i];
  }
  bool operator<(const Version &other) const { return cmp(other) < 0; }
  bool operator>(const Version &other) const { return cmp(other) > 0; }
  bool operator<=(const Version &other) const { return cmp(other) <= 0; }
  bool operator>=(const Version &other) const { return cmp(other) >= 0; }
};


/**
  @class Format_description_log_event

  For binlog version 4.
  This event is saved by threads which read it, as they need it for future
  use (to decode the ordinary events).

  @section Format_description_log_event_binary_format Binary Format
*/

class Format_description_log_event: public Start_log_event_v3
{
public:
  /*
     The size of the fixed header which _all_ events have
     (for binlogs written by this version, this is equal to
     LOG_EVENT_HEADER_LEN), except FORMAT_DESCRIPTION_EVENT and ROTATE_EVENT
     (those have a header of size LOG_EVENT_MINIMAL_HEADER_LEN).
  */
  uint8 common_header_len;
  uint8 number_of_event_types;
  /* 
     The list of post-headers' lengths followed 
     by the checksum alg description byte
  */
  uint8 *post_header_len;
  class master_version_split: public Version {
  public:
    enum {KIND_MYSQL, KIND_MARIADB};
    int kind;
    master_version_split() :kind(KIND_MARIADB) { }
    master_version_split(const char *version);
    bool version_is_valid() const
    {
      /* It is invalid only when all version numbers are 0 */
      return !(m_ver[0] == 0 && m_ver[1] == 0 && m_ver[2] == 0);
    }
  };
  master_version_split server_version_split;
  const uint8 *event_type_permutation;

  Format_description_log_event(uint8 binlog_ver, const char* server_ver=0);
  Format_description_log_event(const uchar *buf, uint event_len,
                               const Format_description_log_event
                               *description_event);
  ~Format_description_log_event()
  {
    my_free(post_header_len);
  }
  Log_event_type get_type_code() { return FORMAT_DESCRIPTION_EVENT;}
#ifdef MYSQL_SERVER
  bool write();
#endif
  bool header_is_valid() const
  {
    return ((common_header_len >= ((binlog_version==1) ? OLD_HEADER_LEN :
                                   LOG_EVENT_MINIMAL_HEADER_LEN)) &&
            (post_header_len != NULL));
  }

  bool is_valid() const
  {
    return header_is_valid() && server_version_split.version_is_valid();
  }

  int get_data_size()
  {
    /*
      The vector of post-header lengths is considered as part of the
      post-header, because in a given version it never changes (contrary to the
      query in a Query_log_event).
    */
    return FORMAT_DESCRIPTION_HEADER_LEN;
  }

  Binlog_crypt_data crypto_data;
  bool start_decryption(Start_encryption_log_event* sele);
  void copy_crypto_data(const Format_description_log_event* o)
  {
    crypto_data= o->crypto_data;
  }
  void reset_crypto()
  {
    crypto_data.scheme= 0;
  }

  void calc_server_version_split();
  static bool is_version_before_checksum(const master_version_split *version_split);
protected:
#if defined(MYSQL_SERVER) && defined(HAVE_REPLICATION)
  virtual int do_apply_event(rpl_group_info *rgi);
  virtual int do_update_pos(rpl_group_info *rgi);
  virtual enum_skip_reason do_shall_skip(rpl_group_info *rgi);
#endif
};


/**
  @class Intvar_log_event

  An Intvar_log_event will be created just before a Query_log_event,
  if the query uses one of the variables LAST_INSERT_ID or INSERT_ID.
  Each Intvar_log_event holds the value of one of these variables.

  @section Intvar_log_event_binary_format Binary Format

  The Post-Header for this event type is empty.  The Body has two
  components:

  <table>
  <caption>Body for Intvar_log_event</caption>

  <tr>
    <th>Name</th>
    <th>Format</th>
    <th>Description</th>
  </tr>

  <tr>
    <td>type</td>
    <td>1 byte enumeration</td>
    <td>One byte identifying the type of variable stored.  Currently,
    two identifiers are supported:  LAST_INSERT_ID_EVENT==1 and
    INSERT_ID_EVENT==2.
    </td>
  </tr>

  <tr>
    <td>value</td>
    <td>8 byte unsigned integer</td>
    <td>The value of the variable.</td>
  </tr>

  </table>
*/
class Intvar_log_event: public Log_event
{
public:
  ulonglong val;
  uchar type;

#ifdef MYSQL_SERVER
Intvar_log_event(THD* thd_arg,uchar type_arg, ulonglong val_arg,
                 bool using_trans, bool direct)
    :Log_event(thd_arg,0,using_trans),val(val_arg),type(type_arg)
  {
    if (direct)
      cache_type= Log_event::EVENT_NO_CACHE;
  }
#ifdef HAVE_REPLICATION
  void pack_info(Protocol* protocol);
#endif /* HAVE_REPLICATION */
#else
  bool print(FILE* file, PRINT_EVENT_INFO* print_event_info);
#endif

  Intvar_log_event(const uchar *buf,
                   const Format_description_log_event *description_event);
  ~Intvar_log_event() {}
  Log_event_type get_type_code() { return INTVAR_EVENT;}
  const char* get_var_type_name();
  int get_data_size() { return  9; /* sizeof(type) + sizeof(val) */;}
#ifdef MYSQL_SERVER
  bool write();
#endif
  bool is_valid() const { return 1; }
  bool is_part_of_group() { return 1; }

private:
#if defined(MYSQL_SERVER) && defined(HAVE_REPLICATION)
  virtual int do_apply_event(rpl_group_info *rgi);
  virtual int do_update_pos(rpl_group_info *rgi);
  virtual enum_skip_reason do_shall_skip(rpl_group_info *rgi);
#endif
};


/**
  @class Rand_log_event

  Logs random seed used by the next RAND(), and by PASSWORD() in 4.1.0.
  4.1.1 does not need it (it's repeatable again) so this event needn't be
  written in 4.1.1 for PASSWORD() (but the fact that it is written is just a
  waste, it does not cause bugs).

  The state of the random number generation consists of 128 bits,
  which are stored internally as two 64-bit numbers.

  @section Rand_log_event_binary_format Binary Format  

  The Post-Header for this event type is empty.  The Body has two
  components:

  <table>
  <caption>Body for Rand_log_event</caption>

  <tr>
    <th>Name</th>
    <th>Format</th>
    <th>Description</th>
  </tr>

  <tr>
    <td>seed1</td>
    <td>8 byte unsigned integer</td>
    <td>64 bit random seed1.</td>
  </tr>

  <tr>
    <td>seed2</td>
    <td>8 byte unsigned integer</td>
    <td>64 bit random seed2.</td>
  </tr>
  </table>
*/

class Rand_log_event: public Log_event
{
 public:
  ulonglong seed1;
  ulonglong seed2;

#ifdef MYSQL_SERVER
  Rand_log_event(THD* thd_arg, ulonglong seed1_arg, ulonglong seed2_arg,
                 bool using_trans, bool direct)
    :Log_event(thd_arg,0,using_trans),seed1(seed1_arg),seed2(seed2_arg)
  {
    if (direct)
      cache_type= Log_event::EVENT_NO_CACHE;
  }
#ifdef HAVE_REPLICATION
  void pack_info(Protocol* protocol);
#endif /* HAVE_REPLICATION */
#else
  bool print(FILE* file, PRINT_EVENT_INFO* print_event_info);
#endif

  Rand_log_event(const uchar *buf,
                 const Format_description_log_event *description_event);
  ~Rand_log_event() {}
  Log_event_type get_type_code() { return RAND_EVENT;}
  int get_data_size() { return 16; /* sizeof(ulonglong) * 2*/ }
#ifdef MYSQL_SERVER
  bool write();
#endif
  bool is_valid() const { return 1; }
  bool is_part_of_group() { return 1; }

private:
#if defined(MYSQL_SERVER) && defined(HAVE_REPLICATION)
  virtual int do_apply_event(rpl_group_info *rgi);
  virtual int do_update_pos(rpl_group_info *rgi);
  virtual enum_skip_reason do_shall_skip(rpl_group_info *rgi);
#endif
};


class Xid_apply_log_event: public Log_event
{
public:
#ifdef MYSQL_SERVER
  Xid_apply_log_event(THD* thd_arg):
   Log_event(thd_arg, 0, TRUE) {}
#endif
  Xid_apply_log_event(const uchar *buf,
                const Format_description_log_event *description_event):
   Log_event(buf, description_event) {}

  ~Xid_apply_log_event() {}
  bool is_valid() const { return 1; }
private:
#if defined(MYSQL_SERVER) && defined(HAVE_REPLICATION)
  virtual int do_commit()= 0;
  virtual int do_apply_event(rpl_group_info *rgi);
  int do_record_gtid(THD *thd, rpl_group_info *rgi, bool in_trans,
                     void **out_hton);
  enum_skip_reason do_shall_skip(rpl_group_info *rgi);
  virtual const char* get_query()= 0;
#endif
};


/**
  @class Xid_log_event

  Logs xid of the transaction-to-be-committed in the 2pc protocol.
  Has no meaning in replication, slaves ignore it.

  @section Xid_log_event_binary_format Binary Format  
*/
#ifdef MYSQL_CLIENT
typedef ulonglong my_xid; // this line is the same as in handler.h
#endif

class Xid_log_event: public Xid_apply_log_event
{
public:
  my_xid xid;

#ifdef MYSQL_SERVER
  Xid_log_event(THD* thd_arg, my_xid x, bool direct):
   Xid_apply_log_event(thd_arg), xid(x)
   {
     if (direct)
       cache_type= Log_event::EVENT_NO_CACHE;
   }
  const char* get_query()
  {
    return "COMMIT /* implicit, from Xid_log_event */";
  }
#ifdef HAVE_REPLICATION
  void pack_info(Protocol* protocol);
#endif /* HAVE_REPLICATION */
#else
  bool print(FILE* file, PRINT_EVENT_INFO* print_event_info);
#endif

  Xid_log_event(const uchar *buf,
                const Format_description_log_event *description_event);
  ~Xid_log_event() {}
  Log_event_type get_type_code() { return XID_EVENT;}
  int get_data_size() { return sizeof(xid); }
#ifdef MYSQL_SERVER
  bool write();
#endif

private:
#if defined(MYSQL_SERVER) && defined(HAVE_REPLICATION)
  int do_commit();
#endif
};


/**
  @class XA_prepare_log_event

  Similar to Xid_log_event except that
  - it is specific to XA transaction
  - it carries out the prepare logics rather than the final committing
    when @c one_phase member is off. The latter option is only for
    compatibility with the upstream.

  From the groupping perspective the event finalizes the current
  "prepare" group that is started with Gtid_log_event similarly to the
  regular replicated transaction.
*/

/**
  Function serializes XID which is characterized by by four last arguments
  of the function.
  Serialized XID is presented in valid hex format and is returned to
  the caller in a buffer pointed by the first argument.
  The buffer size provived by the caller must be not less than
  8 + 2 * XIDDATASIZE +  4 * sizeof(XID::formatID) + 1, see
  {MYSQL_,}XID definitions.

  @param buf  pointer to a buffer allocated for storing serialized data
  @param fmt  formatID value
  @param gln  gtrid_length value
  @param bln  bqual_length value
  @param dat  data value

  @return  the value of the buffer pointer
*/

inline char *serialize_xid(char *buf, long fmt, long gln, long bln,
                           const char *dat)
{
  int i;
  char *c= buf;
  /*
    Build a string consisting of the hex format representation of XID
    as passed through fmt,gln,bln,dat argument:
      X'hex11hex12...hex1m',X'hex21hex22...hex2n',11
    and store it into buf.
  */
  c[0]= 'X';
  c[1]= '\'';
  c+= 2;
  for (i= 0; i < gln; i++)
  {
    c[0]=_dig_vec_lower[((uchar*) dat)[i] >> 4];
    c[1]=_dig_vec_lower[((uchar*) dat)[i] & 0x0f];
    c+= 2;
  }
  c[0]= '\'';
  c[1]= ',';
  c[2]= 'X';
  c[3]= '\'';
  c+= 4;

  for (; i < gln + bln; i++)
  {
    c[0]=_dig_vec_lower[((uchar*) dat)[i] >> 4];
    c[1]=_dig_vec_lower[((uchar*) dat)[i] & 0x0f];
    c+= 2;
  }
  c[0]= '\'';
  sprintf(c+1, ",%lu", fmt);

 return buf;
}

/*
  The size of the string containing serialized Xid representation
  is computed as a sum of
  eight as the number of formatting symbols (X'',X'',)
  plus 2 x XIDDATASIZE (2 due to hex format),
  plus space for decimal digits of XID::formatID,
  plus one for 0x0.
*/
static const uint ser_buf_size=
  8 + 2 * MYSQL_XIDDATASIZE + 4 * sizeof(long) + 1;

struct event_mysql_xid_t :  MYSQL_XID
{
  char buf[ser_buf_size];
  char *serialize()
  {
    return serialize_xid(buf, formatID, gtrid_length, bqual_length, data);
  }
};

#ifndef MYSQL_CLIENT
struct event_xid_t : XID
{
  char buf[ser_buf_size];

  char *serialize(char *buf_arg)
  {
    return serialize_xid(buf_arg, formatID, gtrid_length, bqual_length, data);
  }
  char *serialize()
  {
    return serialize(buf);
  }
};
#endif

class XA_prepare_log_event: public Xid_apply_log_event
{
protected:

  /* Constant contributor to subheader in write() by members of XID struct. */
  static const int xid_subheader_no_data= 12;
  event_mysql_xid_t m_xid;
  void *xid;
  bool one_phase;

public:
#ifdef MYSQL_SERVER
  XA_prepare_log_event(THD* thd_arg, XID *xid_arg, bool one_phase_arg):
    Xid_apply_log_event(thd_arg), xid(xid_arg), one_phase(one_phase_arg)
  {
    cache_type= Log_event::EVENT_NO_CACHE;
  }
#ifdef HAVE_REPLICATION
  void pack_info(Protocol* protocol);
#endif /* HAVE_REPLICATION */
#else
  bool print(FILE* file, PRINT_EVENT_INFO* print_event_info);
#endif
  XA_prepare_log_event(const uchar *buf,
                       const Format_description_log_event *description_event);
  ~XA_prepare_log_event() {}
  Log_event_type get_type_code() { return XA_PREPARE_LOG_EVENT; }
  bool is_valid() const { return m_xid.formatID != -1; }
  int get_data_size()
  {
    return xid_subheader_no_data + m_xid.gtrid_length + m_xid.bqual_length;
  }

#ifdef MYSQL_SERVER
  bool write();
#endif

private:
#if defined(MYSQL_SERVER) && defined(HAVE_REPLICATION)
  char query[sizeof("XA COMMIT ONE PHASE") + 1 + ser_buf_size];
  int do_commit();
  const char* get_query()
  {
    sprintf(query,
            (one_phase ? "XA COMMIT %s ONE PHASE" : "XA PREPARE %s"),
            m_xid.serialize());
    return query;
  }
#endif
};


/**
  @class User_var_log_event

  Every time a query uses the value of a user variable, a User_var_log_event is
  written before the Query_log_event, to set the user variable.

  @section User_var_log_event_binary_format Binary Format  
*/

class User_var_log_event: public Log_event
{
public:
  enum {
    UNDEF_F= 0,
    UNSIGNED_F= 1
  };
  const char *name;
  size_t name_len;
  const char *val;
  size_t val_len;
  Item_result type;
  uint charset_number;
  bool is_null;
  uchar flags;
#ifdef MYSQL_SERVER
  bool deferred;
  query_id_t query_id;
  User_var_log_event(THD* thd_arg, const char *name_arg, size_t name_len_arg,
                     const char *val_arg, size_t val_len_arg,
                     Item_result type_arg,
		     uint charset_number_arg, uchar flags_arg,
                     bool using_trans, bool direct)
    :Log_event(thd_arg, 0, using_trans),
    name(name_arg), name_len(name_len_arg), val(val_arg),
    val_len(val_len_arg), type(type_arg), charset_number(charset_number_arg),
    flags(flags_arg), deferred(false)
    {
      is_null= !val;
      if (direct)
        cache_type= Log_event::EVENT_NO_CACHE;
    }
  void pack_info(Protocol* protocol);
#else
  bool print(FILE* file, PRINT_EVENT_INFO* print_event_info);
#endif

  User_var_log_event(const uchar *buf, uint event_len,
                     const Format_description_log_event *description_event);
  ~User_var_log_event() {}
  Log_event_type get_type_code() { return USER_VAR_EVENT;}
#ifdef MYSQL_SERVER
  bool write();
  /* 
     Getter and setter for deferred User-event. 
     Returns true if the event is not applied directly 
     and which case the applier adjusts execution path.
  */
  bool is_deferred() { return deferred; }
  /*
    In case of the deferred applying the variable instance is flagged
    and the parsing time query id is stored to be used at applying time.
  */
  void set_deferred(query_id_t qid) { deferred= true; query_id= qid; }
#endif
  bool is_valid() const { return name != 0; }
  bool is_part_of_group() { return 1; }

private:
#if defined(MYSQL_SERVER) && defined(HAVE_REPLICATION)
  virtual int do_apply_event(rpl_group_info *rgi);
  virtual int do_update_pos(rpl_group_info *rgi);
  virtual enum_skip_reason do_shall_skip(rpl_group_info *rgi);
#endif
};


/**
  @class Stop_log_event

  @section Stop_log_event_binary_format Binary Format

  The Post-Header and Body for this event type are empty; it only has
  the Common-Header.
*/
class Stop_log_event: public Log_event
{
public:
#ifdef MYSQL_SERVER
  Stop_log_event() :Log_event()
  {}
#else
  bool print(FILE* file, PRINT_EVENT_INFO* print_event_info);
#endif

  Stop_log_event(const uchar *buf,
                 const Format_description_log_event *description_event):
    Log_event(buf, description_event)
  {}
  ~Stop_log_event() {}
  Log_event_type get_type_code() { return STOP_EVENT;}
  bool is_valid() const { return 1; }

private:
#if defined(MYSQL_SERVER) && defined(HAVE_REPLICATION)
  virtual int do_update_pos(rpl_group_info *rgi);
  virtual enum_skip_reason do_shall_skip(rpl_group_info *rgi)
  {
    /*
      Events from ourself should be skipped, but they should not
      decrease the slave skip counter.
     */
    if (this->server_id == global_system_variables.server_id)
      return Log_event::EVENT_SKIP_IGNORE;
    else
      return Log_event::EVENT_SKIP_NOT;
  }
#endif
};

/**
  @class Rotate_log_event

  This will be deprecated when we move to using sequence ids.

  @section Rotate_log_event_binary_format Binary Format

  The Post-Header has one component:

  <table>
  <caption>Post-Header for Rotate_log_event</caption>

  <tr>
    <th>Name</th>
    <th>Format</th>
    <th>Description</th>
  </tr>

  <tr>
    <td>position</td>
    <td>8 byte integer</td>
    <td>The position within the binlog to rotate to.</td>
  </tr>

  </table>

  The Body has one component:

  <table>
  <caption>Body for Rotate_log_event</caption>

  <tr>
    <th>Name</th>
    <th>Format</th>
    <th>Description</th>
  </tr>

  <tr>
    <td>new_log</td>
    <td>variable length string without trailing zero, extending to the
    end of the event (determined by the length field of the
    Common-Header)
    </td>
    <td>Name of the binlog to rotate to.</td>
  </tr>

  </table>
*/

class Rotate_log_event: public Log_event
{
public:
  enum {
    DUP_NAME= 2, // if constructor should dup the string argument
    RELAY_LOG=4  // rotate event for relay log
  };
  const char *new_log_ident;
  ulonglong pos;
  uint ident_len;
  uint flags;
#ifdef MYSQL_SERVER
  Rotate_log_event(const char* new_log_ident_arg,
		   uint ident_len_arg,
		   ulonglong pos_arg, uint flags);
#ifdef HAVE_REPLICATION
  void pack_info(Protocol* protocol);
#endif /* HAVE_REPLICATION */
#else
  bool print(FILE* file, PRINT_EVENT_INFO* print_event_info);
#endif

  Rotate_log_event(const uchar *buf, uint event_len,
                   const Format_description_log_event* description_event);
  ~Rotate_log_event()
  {
    if (flags & DUP_NAME)
      my_free((void*) new_log_ident);
  }
  Log_event_type get_type_code() { return ROTATE_EVENT;}
  my_off_t get_header_len(my_off_t l __attribute__((unused)))
  { return LOG_EVENT_MINIMAL_HEADER_LEN; }
  int get_data_size() { return  ident_len + ROTATE_HEADER_LEN;}
  bool is_valid() const { return new_log_ident != 0; }
#ifdef MYSQL_SERVER
  bool write();
#endif

private:
#if defined(MYSQL_SERVER) && defined(HAVE_REPLICATION)
  virtual int do_update_pos(rpl_group_info *rgi);
  virtual enum_skip_reason do_shall_skip(rpl_group_info *rgi);
#endif
};


class Binlog_checkpoint_log_event: public Log_event
{
public:
  char *binlog_file_name;
  uint binlog_file_len;

#ifdef MYSQL_SERVER
  Binlog_checkpoint_log_event(const char *binlog_file_name_arg,
                              uint binlog_file_len_arg);
#ifdef HAVE_REPLICATION
  void pack_info(Protocol *protocol);
#endif
#else
  bool print(FILE *file, PRINT_EVENT_INFO *print_event_info);
#endif
  Binlog_checkpoint_log_event(const uchar *buf, uint event_len,
                              const Format_description_log_event
                              *description_event);
  ~Binlog_checkpoint_log_event() { my_free(binlog_file_name); }
  Log_event_type get_type_code() { return BINLOG_CHECKPOINT_EVENT;}
  int get_data_size() { return binlog_file_len + BINLOG_CHECKPOINT_HEADER_LEN;}
  bool is_valid() const { return binlog_file_name != 0; }
#ifdef MYSQL_SERVER
  bool write();
  enum_skip_reason do_shall_skip(rpl_group_info *rgi);
#endif
};


/**
  @class Gtid_log_event

  This event is logged as part of every event group to give the global
  transaction id (GTID) of that group.

  It replaces the BEGIN query event used in earlier versions to begin most
  event groups, but is also used for events that used to be stand-alone.

  @section Gtid_log_event_binary_format Binary Format

  The binary format for Gtid_log_event has 6 extra reserved bytes to make the
  length a total of 19 byte (+ 19 bytes of header in common with all events).
  This is just the minimal size for a BEGIN query event, which makes it easy
  to replace this event with such BEGIN event to remain compatible with old
  slave servers.

  <table>
  <caption>Post-Header</caption>

  <tr>
    <th>Name</th>
    <th>Format</th>
    <th>Description</th>
  </tr>

  <tr>
    <td>seq_no</td>
    <td>8 byte unsigned integer</td>
    <td>increasing id within one server_id. Starts at 1, holes in the sequence
        may occur</td>
  </tr>

  <tr>
    <td>domain_id</td>
    <td>4 byte unsigned integer</td>
    <td>Replication domain id, identifying independent replication streams></td>
  </tr>

  <tr>
    <td>flags</td>
    <td>1 byte bitfield</td>
    <td>Bit 0 set indicates stand-alone event (no terminating COMMIT)</td>
    <td>Bit 1 set indicates group commit, and that commit id exists</td>
    <td>Bit 2 set indicates a transactional event group (can be safely rolled
        back).</td>
    <td>Bit 3 set indicates that user allowed optimistic parallel apply (the
        @@SESSION.replicate_allow_parallel value was true at commit).</td>
    <td>Bit 4 set indicates that this transaction encountered a row (or other)
        lock wait during execution.</td>
  </tr>

  <tr>
    <td>Reserved (no group commit) / commit id (group commit) (see flags bit 1)</td>
    <td>6 bytes / 8 bytes</td>
    <td>Reserved bytes, set to 0. Maybe be used for future expansion (no
        group commit). OR commit id, same for all GTIDs in the same group
        commit (see flags bit 1).</td>
  </tr>
  </table>

  The Body of Gtid_log_event is empty. The total event size is 19 bytes +
  the normal 19 bytes common-header.
*/

class Gtid_log_event: public Log_event
{
public:
  uint64 seq_no;
  uint64 commit_id;
  uint32 domain_id;
#ifdef MYSQL_SERVER
  event_xid_t xid;
#else
  event_mysql_xid_t xid;
#endif
  uchar flags2;
  uint  flags_extra; // more flags area placed after the regular flags2's one
  /*
    Number of engine participants in transaction minus 1.
    When zero the event does not contain that information.
  */
  uint8 extra_engines;

  /* Flags2. */

  /* FL_STANDALONE is set when there is no terminating COMMIT event. */
  static const uchar FL_STANDALONE= 1;
  /*
    FL_GROUP_COMMIT_ID is set when event group is part of a group commit on the
    master. Groups with same commit_id are part of the same group commit.
  */
  static const uchar FL_GROUP_COMMIT_ID= 2;
  /*
    FL_TRANSACTIONAL is set for an event group that can be safely rolled back
    (no MyISAM, eg.).
  */
  static const uchar FL_TRANSACTIONAL= 4;
  /*
    FL_ALLOW_PARALLEL reflects the (negation of the) value of
    @@SESSION.skip_parallel_replication at the time of commit.
  */
  static const uchar FL_ALLOW_PARALLEL= 8;
  /*
    FL_WAITED is set if a row lock wait (or other wait) is detected during the
    execution of the transaction.
  */
  static const uchar FL_WAITED= 16;
  /* FL_DDL is set for event group containing DDL. */
  static const uchar FL_DDL= 32;
  /* FL_PREPARED_XA is set for XA transaction. */
  static const uchar FL_PREPARED_XA= 64;
  /* FL_"COMMITTED or ROLLED-BACK"_XA is set for XA transaction. */
  static const uchar FL_COMPLETED_XA= 128;

  /* Flags_extra. */

  /*
    FL_EXTRA_MULTI_ENGINE is set for event group comprising a transaction
    involving multiple storage engines. No flag and extra data are added
    to the event when the transaction involves only one engine.
  */
  static const uchar FL_EXTRA_MULTI_ENGINE= 1;

#ifdef MYSQL_SERVER
  Gtid_log_event(THD *thd_arg, uint64 seq_no, uint32 domain_id, bool standalone,
                 uint16 flags, bool is_transactional, uint64 commit_id,
                 bool has_xid= false, bool is_ro_1pc= false);
#ifdef HAVE_REPLICATION
  void pack_info(Protocol *protocol);
  virtual int do_apply_event(rpl_group_info *rgi);
  virtual int do_update_pos(rpl_group_info *rgi);
  virtual enum_skip_reason do_shall_skip(rpl_group_info *rgi);
#endif
#else
  bool print(FILE *file, PRINT_EVENT_INFO *print_event_info);
#endif
  Gtid_log_event(const uchar *buf, uint event_len,
                 const Format_description_log_event *description_event);
  ~Gtid_log_event() { }
  Log_event_type get_type_code() { return GTID_EVENT; }
  enum_logged_status logged_status() { return LOGGED_NO_DATA; }
  int get_data_size()
  {
    return GTID_HEADER_LEN + ((flags2 & FL_GROUP_COMMIT_ID) ? 2 : 0);
  }
  bool is_valid() const { return seq_no != 0; }
#ifdef MYSQL_SERVER
  bool write();
  static int make_compatible_event(String *packet, bool *need_dummy_event,
                                    ulong ev_offset, enum enum_binlog_checksum_alg checksum_alg);
  static bool peek(const uchar *event_start, size_t event_len,
                   enum enum_binlog_checksum_alg checksum_alg,
                   uint32 *domain_id, uint32 *server_id, uint64 *seq_no,
                   uchar *flags2, const Format_description_log_event *fdev);
#endif
};


/**
  @class Gtid_list_log_event

  This event is logged at the start of every binlog file to record the
  current replication state: the last global transaction id (GTID) applied
  on the server within each replication domain.

  It consists of a list of GTIDs, one for each replication domain ever seen
  on the server.

  @section Gtid_list_log_event_binary_format Binary Format

  <table>
  <caption>Post-Header</caption>

  <tr>
    <th>Name</th>
    <th>Format</th>
    <th>Description</th>
  </tr>

  <tr>
    <td>count</td>
    <td>4 byte unsigned integer</td>
    <td>The lower 28 bits are the number of GTIDs. The upper 4 bits are
        flags bits.</td>
  </tr>
  </table>

  <table>
  <caption>Body</caption>

  <tr>
    <th>Name</th>
    <th>Format</th>
    <th>Description</th>
  </tr>

  <tr>
    <td>domain_id</td>
    <td>4 byte unsigned integer</td>
    <td>Replication domain id of one GTID</td>
  </tr>

  <tr>
    <td>server_id</td>
    <td>4 byte unsigned integer</td>
    <td>Server id of one GTID</td>
  </tr>

  <tr>
    <td>seq_no</td>
    <td>8 byte unsigned integer</td>
    <td>sequence number of one GTID</td>
  </tr>
  </table>

  The three elements in the body repeat COUNT times to form the GTID list.

  At the time of writing, only two flag bit are in use.

  Bit 28 of `count' is used for flag FLAG_UNTIL_REACHED, which is sent in a
  Gtid_list event from the master to the slave to indicate that the START
  SLAVE UNTIL master_gtid_pos=xxx condition has been reached. (This flag is
  only sent in "fake" events generated on the fly, it is not written into
  the binlog).
*/

class Gtid_list_log_event: public Log_event
{
public:
  uint32 count;
  uint32 gl_flags;
  struct rpl_gtid *list;
  uint64 *sub_id_list;

  static const uint element_size= 4+4+8;
  /* Upper bits stored in 'count'. See comment above */
  enum gtid_flags
  {
    FLAG_UNTIL_REACHED= (1<<28),
    FLAG_IGN_GTIDS= (1<<29),
  };
#ifdef MYSQL_SERVER
  Gtid_list_log_event(rpl_binlog_state *gtid_set, uint32 gl_flags);
  Gtid_list_log_event(slave_connection_state *gtid_set, uint32 gl_flags);
#ifdef HAVE_REPLICATION
  void pack_info(Protocol *protocol);
#endif
#else
  bool print(FILE *file, PRINT_EVENT_INFO *print_event_info);
#endif
  Gtid_list_log_event(const uchar *buf, uint event_len,
                      const Format_description_log_event *description_event);
  ~Gtid_list_log_event() { my_free(list); my_free(sub_id_list); }
  Log_event_type get_type_code() { return GTID_LIST_EVENT; }
  int get_data_size() {
    /*
      Replacing with dummy event, needed for older slaves, requires a minimum
      of 6 bytes in the body.
    */
    return (count==0 ?
            GTID_LIST_HEADER_LEN+2 : GTID_LIST_HEADER_LEN+count*element_size);
  }
  bool is_valid() const { return list != NULL; }
#if defined(MYSQL_SERVER) && defined(HAVE_REPLICATION)
  bool to_packet(String *packet);
  bool write();
  virtual int do_apply_event(rpl_group_info *rgi);
  enum_skip_reason do_shall_skip(rpl_group_info *rgi);
#endif
  static bool peek(const char *event_start, size_t event_len,
                   enum enum_binlog_checksum_alg checksum_alg,
                   rpl_gtid **out_gtid_list, uint32 *out_list_len,
                   const Format_description_log_event *fdev);
};


/* the classes below are for the new LOAD DATA INFILE logging */

/**
  @class Create_file_log_event

  @section Create_file_log_event_binary_format Binary Format
*/

class Create_file_log_event: public Load_log_event
{
protected:
  /*
    Pretend we are Load event, so we can write out just
    our Load part - used on the slave when writing event out to
    SQL_LOAD-*.info file
  */
  bool fake_base;
public:
  uchar *block;
  const uchar *event_buf;
  uint block_len;
  uint file_id;
  bool inited_from_old;

#ifdef MYSQL_SERVER
  Create_file_log_event(THD* thd, sql_exchange* ex, const char* db_arg,
			const char* table_name_arg,
			List<Item>& fields_arg,
                        bool is_concurrent_arg,
			enum enum_duplicates handle_dup, bool ignore,
			uchar* block_arg, uint block_len_arg,
			bool using_trans);
#ifdef HAVE_REPLICATION
  void pack_info(Protocol* protocol);
#endif /* HAVE_REPLICATION */
#else
  bool print(FILE* file, PRINT_EVENT_INFO* print_event_info);
  bool print(FILE* file, PRINT_EVENT_INFO* print_event_info,
             bool enable_local);
#endif

  Create_file_log_event(const uchar *buf, uint event_len,
                        const Format_description_log_event* description_event);
  ~Create_file_log_event()
  {
    my_free((void*) event_buf);
  }

  Log_event_type get_type_code()
  {
    return fake_base ? Load_log_event::get_type_code() : CREATE_FILE_EVENT;
  }
  int get_data_size()
  {
    return (fake_base ? Load_log_event::get_data_size() :
	    Load_log_event::get_data_size() +
	    4 + 1 + block_len);
  }
  bool is_valid() const { return inited_from_old || block != 0; }
#ifdef MYSQL_SERVER
  bool write_data_header();
  bool write_data_body();
  /*
    Cut out Create_file extensions and
    write it as Load event - used on the slave
  */
  bool write_base();
#endif

private:
#if defined(MYSQL_SERVER) && defined(HAVE_REPLICATION)
  virtual int do_apply_event(rpl_group_info *rgi);
#endif
};


/**
  @class Append_block_log_event

  @section Append_block_log_event_binary_format Binary Format
*/

class Append_block_log_event: public Log_event
{
public:
  uchar* block;
  uint block_len;
  uint file_id;
  /*
    'db' is filled when the event is created in mysql_load() (the
    event needs to have a 'db' member to be well filtered by
    binlog-*-db rules). 'db' is not written to the binlog (it's not
    used by Append_block_log_event::write()), so it can't be read in
    the Append_block_log_event(const uchar *buf, int event_len)
    constructor.  In other words, 'db' is used only for filtering by
    binlog-*-db rules.  Create_file_log_event is different: it's 'db'
    (which is inherited from Load_log_event) is written to the binlog
    and can be re-read.
  */
  const char* db;

#ifdef MYSQL_SERVER
  Append_block_log_event(THD* thd, const char* db_arg, uchar* block_arg,
			 uint block_len_arg, bool using_trans);
#ifdef HAVE_REPLICATION
  void pack_info(Protocol* protocol);
  virtual int get_create_or_append() const;
#endif /* HAVE_REPLICATION */
#else
  bool print(FILE* file, PRINT_EVENT_INFO* print_event_info);
#endif

  Append_block_log_event(const uchar *buf, uint event_len,
                         const Format_description_log_event
                         *description_event);
  ~Append_block_log_event() {}
  Log_event_type get_type_code() { return APPEND_BLOCK_EVENT;}
  int get_data_size() { return  block_len + APPEND_BLOCK_HEADER_LEN ;}
  bool is_valid() const { return block != 0; }
#ifdef MYSQL_SERVER
  bool write();
  const char* get_db() { return db; }
#endif

private:
#if defined(MYSQL_SERVER) && defined(HAVE_REPLICATION)
  virtual int do_apply_event(rpl_group_info *rgi);
#endif
};


/**
  @class Delete_file_log_event

  @section Delete_file_log_event_binary_format Binary Format
*/

class Delete_file_log_event: public Log_event
{
public:
  uint file_id;
  const char* db; /* see comment in Append_block_log_event */

#ifdef MYSQL_SERVER
  Delete_file_log_event(THD* thd, const char* db_arg, bool using_trans);
#ifdef HAVE_REPLICATION
  void pack_info(Protocol* protocol);
#endif /* HAVE_REPLICATION */
#else
  bool print(FILE* file, PRINT_EVENT_INFO* print_event_info);
  bool print(FILE* file, PRINT_EVENT_INFO* print_event_info,
             bool enable_local);
#endif

  Delete_file_log_event(const uchar *buf, uint event_len,
                        const Format_description_log_event* description_event);
  ~Delete_file_log_event() {}
  Log_event_type get_type_code() { return DELETE_FILE_EVENT;}
  int get_data_size() { return DELETE_FILE_HEADER_LEN ;}
  bool is_valid() const { return file_id != 0; }
#ifdef MYSQL_SERVER
  bool write();
  const char* get_db() { return db; }
#endif

private:
#if defined(MYSQL_SERVER) && defined(HAVE_REPLICATION)
  virtual int do_apply_event(rpl_group_info *rgi);
#endif
};


/**
  @class Execute_load_log_event

  @section Delete_file_log_event_binary_format Binary Format
*/

class Execute_load_log_event: public Log_event
{
public:
  uint file_id;
  const char* db; /* see comment in Append_block_log_event */

#ifdef MYSQL_SERVER
  Execute_load_log_event(THD* thd, const char* db_arg, bool using_trans);
#ifdef HAVE_REPLICATION
  void pack_info(Protocol* protocol);
#endif /* HAVE_REPLICATION */
#else
  bool print(FILE* file, PRINT_EVENT_INFO* print_event_info);
#endif

  Execute_load_log_event(const uchar *buf, uint event_len,
                         const Format_description_log_event
                         *description_event);
  ~Execute_load_log_event() {}
  Log_event_type get_type_code() { return EXEC_LOAD_EVENT;}
  int get_data_size() { return  EXEC_LOAD_HEADER_LEN ;}
  bool is_valid() const { return file_id != 0; }
#ifdef MYSQL_SERVER
  bool write();
  const char* get_db() { return db; }
#endif

private:
#if defined(MYSQL_SERVER) && defined(HAVE_REPLICATION)
  virtual int do_apply_event(rpl_group_info *rgi);
#endif
};


/**
  @class Begin_load_query_log_event

  Event for the first block of file to be loaded, its only difference from
  Append_block event is that this event creates or truncates existing file
  before writing data.

  @section Begin_load_query_log_event_binary_format Binary Format
*/
class Begin_load_query_log_event: public Append_block_log_event
{
public:
#ifdef MYSQL_SERVER
  Begin_load_query_log_event(THD* thd_arg, const char *db_arg,
                             uchar* block_arg, uint block_len_arg,
                             bool using_trans);
#ifdef HAVE_REPLICATION
  Begin_load_query_log_event(THD* thd);
  int get_create_or_append() const;
#endif /* HAVE_REPLICATION */
#endif
  Begin_load_query_log_event(const uchar *buf, uint event_len,
                             const Format_description_log_event
                             *description_event);
  ~Begin_load_query_log_event() {}
  Log_event_type get_type_code() { return BEGIN_LOAD_QUERY_EVENT; }
private:
#if defined(MYSQL_SERVER) && defined(HAVE_REPLICATION)
  virtual enum_skip_reason do_shall_skip(rpl_group_info *rgi);
#endif
};


/*
  Elements of this enum describe how LOAD DATA handles duplicates.
*/
enum enum_load_dup_handling { LOAD_DUP_ERROR= 0, LOAD_DUP_IGNORE,
                              LOAD_DUP_REPLACE };

/**
  @class Execute_load_query_log_event

  Event responsible for LOAD DATA execution, it similar to Query_log_event
  but before executing the query it substitutes original filename in LOAD DATA
  query with name of temporary file.

  @section Execute_load_query_log_event_binary_format Binary Format
*/
class Execute_load_query_log_event: public Query_log_event
{
public:
  uint file_id;       // file_id of temporary file
  uint fn_pos_start;  // pointer to the part of the query that should
                      // be substituted
  uint fn_pos_end;    // pointer to the end of this part of query
  /*
    We have to store type of duplicate handling explicitly, because
    for LOAD DATA it also depends on LOCAL option. And this part
    of query will be rewritten during replication so this information
    may be lost...
  */
  enum_load_dup_handling dup_handling;

#ifdef MYSQL_SERVER
  Execute_load_query_log_event(THD* thd, const char* query_arg,
                               ulong query_length, uint fn_pos_start_arg,
                               uint fn_pos_end_arg,
                               enum_load_dup_handling dup_handling_arg,
                               bool using_trans, bool direct,
                               bool suppress_use, int errcode);
#ifdef HAVE_REPLICATION
  void pack_info(Protocol* protocol);
#endif /* HAVE_REPLICATION */
#else
  bool print(FILE* file, PRINT_EVENT_INFO* print_event_info);
  /* Prints the query as LOAD DATA LOCAL and with rewritten filename */
  bool print(FILE* file, PRINT_EVENT_INFO* print_event_info,
	     const char *local_fname);
#endif
  Execute_load_query_log_event(const uchar *buf, uint event_len,
                               const Format_description_log_event
                               *description_event);
  ~Execute_load_query_log_event() {}

  Log_event_type get_type_code() { return EXECUTE_LOAD_QUERY_EVENT; }
  bool is_valid() const { return Query_log_event::is_valid() && file_id != 0; }

  ulong get_post_header_size_for_derived();
#ifdef MYSQL_SERVER
  bool write_post_header_for_derived();
#endif

private:
#if defined(MYSQL_SERVER) && defined(HAVE_REPLICATION)
  virtual int do_apply_event(rpl_group_info *rgi);
#endif
};


#ifdef MYSQL_CLIENT
/**
  @class Unknown_log_event

  @section Unknown_log_event_binary_format Binary Format
*/
class Unknown_log_event: public Log_event
{
public:
  enum { UNKNOWN, ENCRYPTED } what;
  /*
    Even if this is an unknown event, we still pass description_event to
    Log_event's ctor, this way we can extract maximum information from the
    event's header (the unique ID for example).
  */
  Unknown_log_event(const uchar *buf,
                    const Format_description_log_event *description_event):
    Log_event(buf, description_event), what(UNKNOWN)
  {}
  /* constructor for hopelessly corrupted events */
  Unknown_log_event(): Log_event(), what(ENCRYPTED) {}
  ~Unknown_log_event() {}
  bool print(FILE* file, PRINT_EVENT_INFO* print_event_info);
  Log_event_type get_type_code() { return UNKNOWN_EVENT;}
  bool is_valid() const { return 1; }
};
#endif
char *str_to_hex(char *to, const char *from, size_t len);

/**
  @class Annotate_rows_log_event

  In row-based mode, if binlog_annotate_row_events = ON, each group of
  Table_map_log_events is preceded by an Annotate_rows_log_event which
  contains the query which caused the subsequent rows operations.

  The Annotate_rows_log_event has no post-header and its body contains
  the corresponding query (without trailing zero). Note. The query length
  is to be calculated as a difference between the whole event length and
  the common header length.
*/
class Annotate_rows_log_event: public Log_event
{
public:
#ifndef MYSQL_CLIENT
  Annotate_rows_log_event(THD*, bool using_trans, bool direct);
#endif
  Annotate_rows_log_event(const uchar *buf, uint event_len,
                          const Format_description_log_event*);
  ~Annotate_rows_log_event();

  virtual int get_data_size();
  virtual Log_event_type get_type_code();
  enum_logged_status logged_status() { return LOGGED_NO_DATA; }
  virtual bool is_valid() const;
  virtual bool is_part_of_group() { return 1; }

#ifndef MYSQL_CLIENT
  virtual bool write_data_header();
  virtual bool write_data_body();
#endif

#if !defined(MYSQL_CLIENT) && defined(HAVE_REPLICATION)
  virtual void pack_info(Protocol*);
#endif

#ifdef MYSQL_CLIENT
  virtual bool print(FILE*, PRINT_EVENT_INFO*);
#endif

#if !defined(MYSQL_CLIENT) && defined(HAVE_REPLICATION)
private:
  virtual int do_apply_event(rpl_group_info *rgi);
  virtual int do_update_pos(rpl_group_info *rgi);
  virtual enum_skip_reason do_shall_skip(rpl_group_info*);
#endif

private:
  char *m_query_txt;
  uint  m_query_len;
  char *m_save_thd_query_txt;
  uint  m_save_thd_query_len;
  bool  m_saved_thd_query;
  bool  m_used_query_txt;
};

/**
  @class Table_map_log_event

  In row-based mode, every row operation event is preceded by a
  Table_map_log_event which maps a table definition to a number.  The
  table definition consists of database name, table name, and column
  definitions.

  @section Table_map_log_event_binary_format Binary Format

  The Post-Header has the following components:

  <table>
  <caption>Post-Header for Table_map_log_event</caption>

  <tr>
    <th>Name</th>
    <th>Format</th>
    <th>Description</th>
  </tr>

  <tr>
    <td>table_id</td>
    <td>6 bytes unsigned integer</td>
    <td>The number that identifies the table.</td>
  </tr>

  <tr>
    <td>flags</td>
    <td>2 byte bitfield</td>
    <td>Reserved for future use; currently always 0.</td>
  </tr>

  </table>

  The Body has the following components:

  <table>
  <caption>Body for Table_map_log_event</caption>

  <tr>
    <th>Name</th>
    <th>Format</th>
    <th>Description</th>
  </tr>

  <tr>
    <td>database_name</td>
    <td>one byte string length, followed by null-terminated string</td>
    <td>The name of the database in which the table resides.  The name
    is represented as a one byte unsigned integer representing the
    number of bytes in the name, followed by length bytes containing
    the database name, followed by a terminating 0 byte.  (Note the
    redundancy in the representation of the length.)  </td>
  </tr>

  <tr>
    <td>table_name</td>
    <td>one byte string length, followed by null-terminated string</td>
    <td>The name of the table, encoded the same way as the database
    name above.</td>
  </tr>

  <tr>
    <td>column_count</td>
    <td>@ref packed_integer "Packed Integer"</td>
    <td>The number of columns in the table, represented as a packed
    variable-length integer.</td>
  </tr>

  <tr>
    <td>column_type</td>
    <td>List of column_count 1 byte enumeration values</td>
    <td>The type of each column in the table, listed from left to
    right.  Each byte is mapped to a column type according to the
    enumeration type enum_field_types defined in mysql_com.h.  The
    mapping of types to numbers is listed in the table @ref
    Table_table_map_log_event_column_types "below" (along with
    description of the associated metadata field).  </td>
  </tr>

  <tr>
    <td>metadata_length</td>
    <td>@ref packed_integer "Packed Integer"</td>
    <td>The length of the following metadata block</td>
  </tr>

  <tr>
    <td>metadata</td>
    <td>list of metadata for each column</td>
    <td>For each column from left to right, a chunk of data who's
    length and semantics depends on the type of the column.  The
    length and semantics for the metadata for each column are listed
    in the table @ref Table_table_map_log_event_column_types
    "below".</td>
  </tr>

  <tr>
    <td>null_bits</td>
    <td>column_count bits, rounded up to nearest byte</td>
    <td>For each column, a bit indicating whether data in the column
    can be NULL or not.  The number of bytes needed for this is
    int((column_count+7)/8).  The flag for the first column from the
    left is in the least-significant bit of the first byte, the second
    is in the second least significant bit of the first byte, the
    ninth is in the least significant bit of the second byte, and so
    on.  </td>
  </tr>
  <tr>
    <td>optional metadata fields</td>
    <td>optional metadata fields are stored in Type, Length, Value(TLV) format.
    Type takes 1 byte. Length is a packed integer value. Values takes
    Length bytes.
    </td>
    <td>There are some optional metadata defined. They are listed in the table
    @ref Table_table_map_event_optional_metadata. Optional metadata fields
    follow null_bits. Whether binlogging an optional metadata is decided by the
    server. The order is not defined, so they can be binlogged in any order.
    </td>
  </tr>

  </table>

  The table below lists all column types, along with the numerical
  identifier for it and the size and interpretation of meta-data used
  to describe the type.

  @anchor Table_table_map_log_event_column_types
  <table>
  <caption>Table_map_log_event column types: numerical identifier and
  metadata</caption>
  <tr>
    <th>Name</th>
    <th>Identifier</th>
    <th>Size of metadata in bytes</th>
    <th>Description of metadata</th>
  </tr>

  <tr>
    <td>MYSQL_TYPE_DECIMAL</td><td>0</td>
    <td>0</td>
    <td>No column metadata.</td>
  </tr>

  <tr>
    <td>MYSQL_TYPE_TINY</td><td>1</td>
    <td>0</td>
    <td>No column metadata.</td>
  </tr>

  <tr>
    <td>MYSQL_TYPE_SHORT</td><td>2</td>
    <td>0</td>
    <td>No column metadata.</td>
  </tr>

  <tr>
    <td>MYSQL_TYPE_LONG</td><td>3</td>
    <td>0</td>
    <td>No column metadata.</td>
  </tr>

  <tr>
    <td>MYSQL_TYPE_FLOAT</td><td>4</td>
    <td>1 byte</td>
    <td>1 byte unsigned integer, representing the "pack_length", which
    is equal to sizeof(float) on the server from which the event
    originates.</td>
  </tr>

  <tr>
    <td>MYSQL_TYPE_DOUBLE</td><td>5</td>
    <td>1 byte</td>
    <td>1 byte unsigned integer, representing the "pack_length", which
    is equal to sizeof(double) on the server from which the event
    originates.</td>
  </tr>

  <tr>
    <td>MYSQL_TYPE_NULL</td><td>6</td>
    <td>0</td>
    <td>No column metadata.</td>
  </tr>

  <tr>
    <td>MYSQL_TYPE_TIMESTAMP</td><td>7</td>
    <td>0</td>
    <td>No column metadata.</td>
  </tr>

  <tr>
    <td>MYSQL_TYPE_LONGLONG</td><td>8</td>
    <td>0</td>
    <td>No column metadata.</td>
  </tr>

  <tr>
    <td>MYSQL_TYPE_INT24</td><td>9</td>
    <td>0</td>
    <td>No column metadata.</td>
  </tr>

  <tr>
    <td>MYSQL_TYPE_DATE</td><td>10</td>
    <td>0</td>
    <td>No column metadata.</td>
  </tr>

  <tr>
    <td>MYSQL_TYPE_TIME</td><td>11</td>
    <td>0</td>
    <td>No column metadata.</td>
  </tr>

  <tr>
    <td>MYSQL_TYPE_DATETIME</td><td>12</td>
    <td>0</td>
    <td>No column metadata.</td>
  </tr>

  <tr>
    <td>MYSQL_TYPE_YEAR</td><td>13</td>
    <td>0</td>
    <td>No column metadata.</td>
  </tr>

  <tr>
    <td><i>MYSQL_TYPE_NEWDATE</i></td><td><i>14</i></td>
    <td>&ndash;</td>
    <td><i>This enumeration value is only used internally and cannot
    exist in a binlog.</i></td>
  </tr>

  <tr>
    <td>MYSQL_TYPE_VARCHAR</td><td>15</td>
    <td>2 bytes</td>
    <td>2 byte unsigned integer representing the maximum length of
    the string.</td>
  </tr>

  <tr>
    <td>MYSQL_TYPE_BIT</td><td>16</td>
    <td>2 bytes</td>
    <td>A 1 byte unsigned int representing the length in bits of the
    bitfield (0 to 64), followed by a 1 byte unsigned int
    representing the number of bytes occupied by the bitfield.  The
    number of bytes is either int((length+7)/8) or int(length/8).</td>
  </tr>

  <tr>
    <td>MYSQL_TYPE_NEWDECIMAL</td><td>246</td>
    <td>2 bytes</td>
    <td>A 1 byte unsigned int representing the precision, followed
    by a 1 byte unsigned int representing the number of decimals.</td>
  </tr>

  <tr>
    <td><i>MYSQL_TYPE_ENUM</i></td><td><i>247</i></td>
    <td>&ndash;</td>
    <td><i>This enumeration value is only used internally and cannot
    exist in a binlog.</i></td>
  </tr>

  <tr>
    <td><i>MYSQL_TYPE_SET</i></td><td><i>248</i></td>
    <td>&ndash;</td>
    <td><i>This enumeration value is only used internally and cannot
    exist in a binlog.</i></td>
  </tr>

  <tr>
    <td>MYSQL_TYPE_TINY_BLOB</td><td>249</td>
    <td>&ndash;</td>
    <td><i>This enumeration value is only used internally and cannot
    exist in a binlog.</i></td>
  </tr>

  <tr>
    <td><i>MYSQL_TYPE_MEDIUM_BLOB</i></td><td><i>250</i></td>
    <td>&ndash;</td>
    <td><i>This enumeration value is only used internally and cannot
    exist in a binlog.</i></td>
  </tr>

  <tr>
    <td><i>MYSQL_TYPE_LONG_BLOB</i></td><td><i>251</i></td>
    <td>&ndash;</td>
    <td><i>This enumeration value is only used internally and cannot
    exist in a binlog.</i></td>
  </tr>

  <tr>
    <td>MYSQL_TYPE_BLOB</td><td>252</td>
    <td>1 byte</td>
    <td>The pack length, i.e., the number of bytes needed to represent
    the length of the blob: 1, 2, 3, or 4.</td>
  </tr>

  <tr>
    <td>MYSQL_TYPE_VAR_STRING</td><td>253</td>
    <td>2 bytes</td>
    <td>This is used to store both strings and enumeration values.
    The first byte is a enumeration value storing the <i>real
    type</i>, which may be either MYSQL_TYPE_VAR_STRING or
    MYSQL_TYPE_ENUM.  The second byte is a 1 byte unsigned integer
    representing the field size, i.e., the number of bytes needed to
    store the length of the string.</td>
  </tr>

  <tr>
    <td>MYSQL_TYPE_STRING</td><td>254</td>
    <td>2 bytes</td>
    <td>The first byte is always MYSQL_TYPE_VAR_STRING (i.e., 253).
    The second byte is the field size, i.e., the number of bytes in
    the representation of size of the string: 3 or 4.</td>
  </tr>

  <tr>
    <td>MYSQL_TYPE_GEOMETRY</td><td>255</td>
    <td>1 byte</td>
    <td>The pack length, i.e., the number of bytes needed to represent
    the length of the geometry: 1, 2, 3, or 4.</td>
  </tr>

  </table>
  The table below lists all optional metadata types, along with the numerical
  identifier for it and the size and interpretation of meta-data used
  to describe the type.

  @anchor Table_table_map_event_optional_metadata
  <table>
  <caption>Table_map_event optional metadata types: numerical identifier and
  metadata. Optional metadata fields are stored in TLV fields.
  Format of values are described in this table. </caption>
  <tr>
    <th>Type</th>
    <th>Description</th>
    <th>Format</th>
  </tr>
  <tr>
    <td>SIGNEDNESS</td>
    <td>signedness of numeric colums. This is included for all values of
    binlog_row_metadata.</td>
    <td>For each numeric column, a bit indicates whether the numeric
    colunm has unsigned flag. 1 means it is unsigned. The number of
    bytes needed for this is int((column_count + 7) / 8). The order is
    the same as the order of column_type field.</td>
  </tr>
  <tr>
    <td>DEFAULT_CHARSET</td>
    <td>Charsets of character columns. It has a default charset for
    the case that most of character columns have same charset and the
    most used charset is binlogged as default charset.Collation
    numbers are binlogged for identifying charsets. They are stored in
    packed length format.  Either DEFAULT_CHARSET or COLUMN_CHARSET is
    included for all values of binlog_row_metadata.</td>
    <td>Default charset's collation is logged first. The charsets which are not
    same to default charset are logged following default charset. They are
    logged as column index and charset collation number pair sequence. The
    column index is counted only in all character columns. The order is same to
    the order of column_type
    field. </td>
  </tr>
  <tr>
    <td>COLUMN_CHARSET</td>
    <td>Charsets of character columns. For the case that most of columns have
    different charsets, this field is logged. It is never logged with
    DEFAULT_CHARSET together.  Either DEFAULT_CHARSET or COLUMN_CHARSET is
    included for all values of binlog_row_metadata.</td>
    <td>It is a collation number sequence for all character columns.</td>
  </tr>
  <tr>
    <td>COLUMN_NAME</td>
    <td>Names of columns. This is only included if
    binlog_row_metadata=FULL.</td>
    <td>A sequence of column names. For each column name, 1 byte for
    the string length in bytes is followed by a string without null
    terminator.</td>
  </tr>
  <tr>
    <td>SET_STR_VALUE</td>
    <td>The string values of SET columns. This is only included if
    binlog_row_metadata=FULL.</td>
    <td>For each SET column, a pack_length representing the value
    count is followed by a sequence of length and string pairs. length
    is the byte count in pack_length format. The string has no null
    terminator.</td>
  </tr>
  <tr>
    <td>ENUM_STR_VALUE</td>
    <td>The string values is ENUM columns. This is only included
    if binlog_row_metadata=FULL.</td>
    <td>The format is the same as SET_STR_VALUE.</td>
  </tr>
  <tr>
    <td>GEOMETRY_TYPE</td>
    <td>The real type of geometry columns. This is only included
    if binlog_row_metadata=FULL.</td>
    <td>A sequence of real type of geometry columns are stored in pack_length
    format. </td>
  </tr>
  <tr>
    <td>SIMPLE_PRIMARY_KEY</td>
    <td>The primary key without any prefix. This is only included
    if binlog_row_metadata=FULL and there is a primary key where every
    key part covers an entire column.</td>
    <td>A sequence of column indexes. The indexes are stored in pack_length
    format.</td>
  </tr>
  <tr>
    <td>PRIMARY_KEY_WITH_PREFIX</td>
    <td>The primary key with some prefix. It doesn't appear together with
    SIMPLE_PRIMARY_KEY. This is only included if
    binlog_row_metadata=FULL and there is a primary key where some key
    part covers a prefix of the column.</td>
    <td>A sequence of column index and prefix length pairs. Both
    column index and prefix length are in pack_length format. Prefix length
    0 means that the whole column value is used.</td>
  </tr>
  <tr>
    <td>ENUM_AND_SET_DEFAULT_CHARSET</td>
    <td>Charsets of ENUM and SET columns. It has the same layout as
    DEFAULT_CHARSET.  If there are SET or ENUM columns and
    binlog_row_metadata=FULL, exactly one of
    ENUM_AND_SET_DEFAULT_CHARSET and ENUM_AND_SET_COLUMN_CHARSET
    appears (the encoder chooses the representation that uses the
    least amount of space).  Otherwise, none of them appears.</td>
    <td>The same format as for DEFAULT_CHARSET, except it counts ENUM
    and SET columns rather than character columns.</td>
  </tr>
  <tr>
    <td>ENUM_AND_SET_COLUMN_CHARSET</td>
    <td>Charsets of ENUM and SET columns. It has the same layout as
    COLUMN_CHARSET.  If there are SET or ENUM columns and
    binlog_row_metadata=FULL, exactly one of
    ENUM_AND_SET_DEFAULT_CHARSET and ENUM_AND_SET_COLUMN_CHARSET
    appears (the encoder chooses the representation that uses the
    least amount of space).  Otherwise, none of them appears.</td>
    <td>The same format as for COLUMN_CHARSET, except it counts ENUM
    and SET columns rather than character columns.</td>
  </tr>
  </table>
*/
class Table_map_log_event : public Log_event
{
public:
  /* Constants */
  enum
  {
    TYPE_CODE = TABLE_MAP_EVENT
  };

  /**
     Enumeration of the errors that can be returned.
   */
  enum enum_error
  {
    ERR_OPEN_FAILURE = -1,               /**< Failure to open table */
    ERR_OK = 0,                                 /**< No error */
    ERR_TABLE_LIMIT_EXCEEDED = 1,      /**< No more room for tables */
    ERR_OUT_OF_MEM = 2,                         /**< Out of memory */
    ERR_BAD_TABLE_DEF = 3,     /**< Table definition does not match */
    ERR_RBR_TO_SBR = 4  /**< daisy-chanining RBR to SBR not allowed */
  };

  enum enum_flag
  {
    /* 
       Nothing here right now, but the flags support is there in
       preparation for changes that are coming.  Need to add a
       constant to make it compile under HP-UX: aCC does not like
       empty enumerations.
    */
    ENUM_FLAG_COUNT
  };

  typedef uint16 flag_set;
  /**
    DEFAULT_CHARSET and COLUMN_CHARSET don't appear together, and
    ENUM_AND_SET_DEFAULT_CHARSET and ENUM_AND_SET_COLUMN_CHARSET don't
    appear together. They are just alternative ways to pack character
    set information. When binlogging, it logs character sets in the
    way that occupies least storage.

    SIMPLE_PRIMARY_KEY and PRIMARY_KEY_WITH_PREFIX don't appear together.
    SIMPLE_PRIMARY_KEY is for the primary keys which only use whole values of
    pk columns. PRIMARY_KEY_WITH_PREFIX is
    for the primary keys which just use part value of pk columns.
   */
  enum Optional_metadata_field_type
  {
    SIGNEDNESS = 1,  // UNSIGNED flag of numeric columns
    DEFAULT_CHARSET, /* Character set of string columns, optimized to
                        minimize space when many columns have the
                        same charset. */
    COLUMN_CHARSET,  /* Character set of string columns, optimized to
                        minimize space when columns have many
                        different charsets. */
    COLUMN_NAME,
    SET_STR_VALUE,                // String value of SET columns
    ENUM_STR_VALUE,               // String value of ENUM columns
    GEOMETRY_TYPE,                // Real type of geometry columns
    SIMPLE_PRIMARY_KEY,           // Primary key without prefix
    PRIMARY_KEY_WITH_PREFIX,      // Primary key with prefix
    ENUM_AND_SET_DEFAULT_CHARSET, /* Character set of enum and set
                                     columns, optimized to minimize
                                     space when many columns have the
                                     same charset. */
    ENUM_AND_SET_COLUMN_CHARSET,  /* Character set of enum and set
                                     columns, optimized to minimize
                                     space when many columns have the
                                     same charset. */
  };
  /**
    Metadata_fields organizes m_optional_metadata into a structured format which
    is easy to access.
  */
  // Values for binlog_row_metadata sysvar
  enum enum_binlog_row_metadata
  {
    BINLOG_ROW_METADATA_NO_LOG= 0,
    BINLOG_ROW_METADATA_MINIMAL= 1,
    BINLOG_ROW_METADATA_FULL= 2
  };
  struct Optional_metadata_fields
  {
    typedef std::pair<unsigned int, unsigned int> uint_pair;
    typedef std::vector<std::string> str_vector;

    struct Default_charset
    {
      Default_charset() : default_charset(0) {}
      bool empty() const { return default_charset == 0; }

      // Default charset for the columns which are not in charset_pairs.
      unsigned int default_charset;

      /* The uint_pair means <column index, column charset number>. */
      std::vector<uint_pair> charset_pairs;
    };

    // Contents of DEFAULT_CHARSET field is converted into Default_charset.
    Default_charset m_default_charset;
    // Contents of ENUM_AND_SET_DEFAULT_CHARSET are converted into
    // Default_charset.
    Default_charset m_enum_and_set_default_charset;
    std::vector<bool> m_signedness;
    // Character set number of every string column
    std::vector<unsigned int> m_column_charset;
    // Character set number of every ENUM or SET column.
    std::vector<unsigned int> m_enum_and_set_column_charset;
    std::vector<std::string> m_column_name;
    // each str_vector stores values of one enum/set column
    std::vector<str_vector> m_enum_str_value;
    std::vector<str_vector> m_set_str_value;
    std::vector<unsigned int> m_geometry_type;
    /*
      The uint_pair means <column index, prefix length>.  Prefix length is 0 if
      whole column value is used.
    */
    std::vector<uint_pair> m_primary_key;

    /*
      It parses m_optional_metadata and populates into above variables.

      @param[in] optional_metadata points to the begin of optional metadata
                                   fields in table_map_event.
      @param[in] optional_metadata_len length of optional_metadata field.
     */
    Optional_metadata_fields(unsigned char* optional_metadata,
                             unsigned int optional_metadata_len);
  };

  /**
    Print column metadata. Its format looks like:
    # Columns(colume_name type, colume_name type, ...)
    if colume_name field is not logged into table_map_log_event, then
    only type is printed.

    @@param[out] file the place where colume metadata is printed
    @@param[in]  The metadata extracted from optional metadata fields
 */
  void print_columns(IO_CACHE *file,
                     const Optional_metadata_fields &fields);
  /**
    Print primary information. Its format looks like:
    # Primary Key(colume_name, column_name(prifix), ...)
    if colume_name field is not logged into table_map_log_event, then
    colume index is printed.

    @@param[out] file the place where primary key is printed
    @@param[in]  The metadata extracted from optional metadata fields
 */
  void print_primary_key(IO_CACHE *file,
                         const Optional_metadata_fields &fields);

  /* Special constants representing sets of flags */
  enum 
  {
    TM_NO_FLAGS = 0U,
    TM_BIT_LEN_EXACT_F = (1U << 0),
    // MariaDB flags (we starts from the other end)
    TM_BIT_HAS_TRIGGERS_F= (1U << 14)
  };

  flag_set get_flags(flag_set flag) const { return m_flags & flag; }

#ifdef MYSQL_SERVER
  Table_map_log_event(THD *thd, TABLE *tbl, ulong tid, bool is_transactional);
#endif
#ifdef HAVE_REPLICATION
  Table_map_log_event(const uchar *buf, uint event_len,
                      const Format_description_log_event *description_event);
#endif

  ~Table_map_log_event();

#ifdef MYSQL_CLIENT
  table_def *create_table_def()
  {
    return new table_def(m_coltype, m_colcnt, m_field_metadata,
                         m_field_metadata_size, m_null_bits, m_flags);
  }
  int rewrite_db(const char* new_name, size_t new_name_len,
                 const Format_description_log_event*);
#endif
  ulonglong get_table_id() const        { return m_table_id; }
  const char *get_table_name() const { return m_tblnam; }
  const char *get_db_name() const    { return m_dbnam; }

  virtual Log_event_type get_type_code() { return TABLE_MAP_EVENT; }
  virtual enum_logged_status logged_status() { return LOGGED_TABLE_MAP; }
  virtual bool is_valid() const { return m_memory != NULL; /* we check malloc */ }
  virtual bool is_part_of_group() { return 1; }

  virtual int get_data_size() { return (uint) m_data_size; } 
#ifdef MYSQL_SERVER
  virtual int save_field_metadata();
  virtual bool write_data_header();
  virtual bool write_data_body();
  virtual const char *get_db() { return m_dbnam; }
#endif

#if defined(MYSQL_SERVER) && defined(HAVE_REPLICATION)
  virtual void pack_info(Protocol *protocol);
#endif

#ifdef MYSQL_CLIENT
  virtual bool print(FILE *file, PRINT_EVENT_INFO *print_event_info);
#endif


private:
#if defined(MYSQL_SERVER) && defined(HAVE_REPLICATION)
  virtual int do_apply_event(rpl_group_info *rgi);
  virtual int do_update_pos(rpl_group_info *rgi);
  virtual enum_skip_reason do_shall_skip(rpl_group_info *rgi);
#endif

#ifdef MYSQL_SERVER
  TABLE         *m_table;
  Binlog_type_info *binlog_type_info_array;


  // Metadata fields buffer
  StringBuffer<1024> m_metadata_buf;

  /**
    Capture the optional metadata fields which should be logged into
    table_map_log_event and serialize them into m_metadata_buf.
  */
  void init_metadata_fields();
  bool init_signedness_field();
  /**
    Capture and serialize character sets.  Character sets for
    character columns (TEXT etc) and character sets for ENUM and SET
    columns are stored in different metadata fields. The reason is
    that TEXT character sets are included even when
    binlog_row_metadata=MINIMAL, whereas ENUM and SET character sets
    are included only when binlog_row_metadata=FULL.

    @param include_type Predicate to determine if a given Field object
    is to be included in the metadata field.

    @param default_charset_type Type code when storing in "default
    charset" format.  (See comment above Table_maps_log_event in
    libbinlogevents/include/rows_event.h)

    @param column_charset_type Type code when storing in "column
    charset" format.  (See comment above Table_maps_log_event in
    libbinlogevents/include/rows_event.h)
  */
  bool init_charset_field(bool(* include_type)(Binlog_type_info *, Field *),
                          Optional_metadata_field_type default_charset_type,
                          Optional_metadata_field_type column_charset_type);
  bool init_column_name_field();
  bool init_set_str_value_field();
  bool init_enum_str_value_field();
  bool init_geometry_type_field();
  bool init_primary_key_field();
#endif

#ifdef MYSQL_CLIENT
  class Charset_iterator;
  class Default_charset_iterator;
  class Column_charset_iterator;
#endif
  char const    *m_dbnam;
  size_t         m_dblen;
  char const    *m_tblnam;
  size_t         m_tbllen;
  ulong          m_colcnt;
  uchar         *m_coltype;

  uchar         *m_memory;
  ulonglong      m_table_id;
  flag_set       m_flags;

  size_t         m_data_size;

  uchar          *m_field_metadata;        // buffer for field metadata
  /*
    The size of field metadata buffer set by calling save_field_metadata()
  */
  ulong          m_field_metadata_size;   
  uchar         *m_null_bits;
  uchar         *m_meta_memory;
  unsigned int   m_optional_metadata_len;
  unsigned char *m_optional_metadata;
};


/**
  @class Rows_log_event

 Common base class for all row-containing log events.

 RESPONSIBILITIES

   Encode the common parts of all events containing rows, which are:
   - Write data header and data body to an IO_CACHE.
   - Provide an interface for adding an individual row to the event.

  @section Rows_log_event_binary_format Binary Format
*/


class Rows_log_event : public Log_event
{
public:
  /**
     Enumeration of the errors that can be returned.
   */
  enum enum_error
  {
    ERR_OPEN_FAILURE = -1,               /**< Failure to open table */
    ERR_OK = 0,                                 /**< No error */
    ERR_TABLE_LIMIT_EXCEEDED = 1,      /**< No more room for tables */
    ERR_OUT_OF_MEM = 2,                         /**< Out of memory */
    ERR_BAD_TABLE_DEF = 3,     /**< Table definition does not match */
    ERR_RBR_TO_SBR = 4  /**< daisy-chanining RBR to SBR not allowed */
  };

  /*
    These definitions allow you to combine the flags into an
    appropriate flag set using the normal bitwise operators.  The
    implicit conversion from an enum-constant to an integer is
    accepted by the compiler, which is then used to set the real set
    of flags.
  */
  enum enum_flag
  {
    /* Last event of a statement */
    STMT_END_F = (1U << 0),

    /* Value of the OPTION_NO_FOREIGN_KEY_CHECKS flag in thd->options */
    NO_FOREIGN_KEY_CHECKS_F = (1U << 1),

    /* Value of the OPTION_RELAXED_UNIQUE_CHECKS flag in thd->options */
    RELAXED_UNIQUE_CHECKS_F = (1U << 2),

    /** 
      Indicates that rows in this event are complete, that is contain
      values for all columns of the table.
     */
    COMPLETE_ROWS_F = (1U << 3),

    /* Value of the OPTION_NO_CHECK_CONSTRAINT_CHECKS flag in thd->options */
    NO_CHECK_CONSTRAINT_CHECKS_F = (1U << 7)
  };

  typedef uint16 flag_set;

  /* Special constants representing sets of flags */
  enum 
  {
      RLE_NO_FLAGS = 0U
  };

  virtual ~Rows_log_event();

  void set_flags(flag_set flags_arg) { m_flags |= flags_arg; }
  void clear_flags(flag_set flags_arg) { m_flags &= ~flags_arg; }
  flag_set get_flags(flag_set flags_arg) const { return m_flags & flags_arg; }
  void update_flags() { int2store(temp_buf + m_flags_pos, m_flags); }

  Log_event_type get_type_code() { return m_type; } /* Specific type (_V1 etc) */
  enum_logged_status logged_status() { return LOGGED_ROW_EVENT; }
  virtual Log_event_type get_general_type_code() = 0; /* General rows op type, no version */

#if defined(MYSQL_SERVER) && defined(HAVE_REPLICATION)
  virtual void pack_info(Protocol *protocol);
#endif

#ifdef MYSQL_CLIENT
  /* not for direct call, each derived has its own ::print() */
  virtual bool print(FILE *file, PRINT_EVENT_INFO *print_event_info)= 0;
  void change_to_flashback_event(PRINT_EVENT_INFO *print_event_info, uchar *rows_buff, Log_event_type ev_type);
  bool print_verbose(IO_CACHE *file,
                     PRINT_EVENT_INFO *print_event_info);
  size_t print_verbose_one_row(IO_CACHE *file, table_def *td,
                               PRINT_EVENT_INFO *print_event_info,
                               MY_BITMAP *cols_bitmap,
                               const uchar *ptr, const uchar *prefix,
                               const my_bool no_fill_output= 0); // if no_fill_output=1, then print result is unnecessary
  size_t calc_row_event_length(table_def *td,
                               PRINT_EVENT_INFO *print_event_info,
                               MY_BITMAP *cols_bitmap,
                               const uchar *value);
  void count_row_events(PRINT_EVENT_INFO *print_event_info);

#endif

#ifdef MYSQL_SERVER
  int add_row_data(uchar *data, size_t length)
  {
    return do_add_row_data(data,length); 
  }
#endif

  /* Member functions to implement superclass interface */
  virtual int get_data_size();

  MY_BITMAP const *get_cols() const { return &m_cols; }
  MY_BITMAP const *get_cols_ai() const { return &m_cols_ai; }
  size_t get_width() const          { return m_width; }
  ulonglong get_table_id() const        { return m_table_id; }

#if defined(MYSQL_SERVER)
  /*
    This member function compares the table's read/write_set
    with this event's m_cols and m_cols_ai. Comparison takes
    into account what type of rows event is this: Delete, Write or
    Update, therefore it uses the correct m_cols[_ai] according
    to the event type code.

    Note that this member function should only be called for the
    following events:
    - Delete_rows_log_event
    - Write_rows_log_event
    - Update_rows_log_event

    @param[IN] table The table to compare this events bitmaps
                     against.

    @return TRUE if sets match, FALSE otherwise. (following
                 bitmap_cmp return logic).

   */
  bool read_write_bitmaps_cmp(TABLE *table)
  {
    bool res= FALSE;

    switch (get_general_type_code())
    {
      case DELETE_ROWS_EVENT:
        res= bitmap_cmp(get_cols(), table->read_set);
        break;
      case UPDATE_ROWS_EVENT:
        res= (bitmap_cmp(get_cols(), table->read_set) &&
              bitmap_cmp(get_cols_ai(), table->rpl_write_set));
        break;
      case WRITE_ROWS_EVENT:
        res= bitmap_cmp(get_cols(), table->rpl_write_set);
        break;
      default:
        /*
          We should just compare bitmaps for Delete, Write
          or Update rows events.
        */
        DBUG_ASSERT(0);
    }
    return res;
  }
#endif

#ifdef MYSQL_SERVER
  virtual bool write_data_header();
  virtual bool write_data_body();
  virtual bool write_compressed();
  virtual const char *get_db() { return m_table->s->db.str; }
#endif
  /*
    Check that malloc() succeeded in allocating memory for the rows
    buffer and the COLS vector. Checking that an Update_rows_log_event
    is valid is done in the Update_rows_log_event::is_valid()
    function.
  */
  virtual bool is_valid() const
  {
    return m_rows_buf && m_cols.bitmap;
  }
  bool is_part_of_group() { return get_flags(STMT_END_F) != 0; }

  uint     m_row_count;         /* The number of rows added to the event */

  const uchar* get_extra_row_data() const   { return m_extra_row_data; }

#if defined(MYSQL_SERVER) && defined(HAVE_REPLICATION)
  virtual uint8 get_trg_event_map()= 0;

  inline bool do_invoke_trigger()
  {
    return (slave_run_triggers_for_rbr && !master_had_triggers) ||
            slave_run_triggers_for_rbr == SLAVE_RUN_TRIGGERS_FOR_RBR_ENFORCE;
  }
#endif

protected:
  /* 
     The constructors are protected since you're supposed to inherit
     this class, not create instances of this class.
  */
#ifdef MYSQL_SERVER
  Rows_log_event(THD*, TABLE*, ulong table_id,
		 MY_BITMAP const *cols, bool is_transactional,
		 Log_event_type event_type);
#endif
  Rows_log_event(const uchar *row_data, uint event_len,
		 const Format_description_log_event *description_event);
  void uncompress_buf();

#ifdef MYSQL_CLIENT
  bool print_helper(FILE *, PRINT_EVENT_INFO *, char const *const name);
#endif

#ifdef MYSQL_SERVER
  virtual int do_add_row_data(uchar *data, size_t length);
#endif

#ifdef MYSQL_SERVER
  TABLE *m_table;		/* The table the rows belong to */
#endif
  ulonglong       m_table_id;	/* Table ID */
  MY_BITMAP   m_cols;		/* Bitmap denoting columns available */
  ulong       m_width;          /* The width of the columns bitmap */
  /*
    Bitmap for columns available in the after image, if present. These
    fields are only available for Update_rows events. Observe that the
    width of both the before image COLS vector and the after image
    COLS vector is the same: the number of columns of the table on the
    master.
  */
  MY_BITMAP   m_cols_ai;

  ulong       m_master_reclength; /* Length of record on master side */

  /* Bit buffers in the same memory as the class */
  uint32    m_bitbuf[128/(sizeof(uint32)*8)];
  uint32    m_bitbuf_ai[128/(sizeof(uint32)*8)];

  uchar    *m_rows_buf;		/* The rows in packed format */
  uchar    *m_rows_cur;		/* One-after the end of the data */
  uchar    *m_rows_end;		/* One-after the end of the allocated space */

  size_t   m_rows_before_size;  /* The length before m_rows_buf */
  size_t   m_flags_pos; /* The position of the m_flags */

  flag_set m_flags;		/* Flags for row-level events */

  Log_event_type m_type;        /* Actual event type */

  uchar    *m_extra_row_data;   /* Pointer to extra row data if any */
                                /* If non null, first byte is length */

  bool m_vers_from_plain;


  /* helper functions */

#if defined(MYSQL_SERVER) && defined(HAVE_REPLICATION)
  const uchar *m_curr_row;     /* Start of the row being processed */
  const uchar *m_curr_row_end; /* One-after the end of the current row */
  uchar    *m_key;      /* Buffer to keep key value during searches */
  KEY      *m_key_info; /* Pointer to KEY info for m_key_nr */
  uint      m_key_nr;   /* Key number */
  bool master_had_triggers;     /* set after tables opening */

  int find_key(); // Find a best key to use in find_row()
  int find_row(rpl_group_info *);
  int write_row(rpl_group_info *, const bool);
  int update_sequence();

  // Unpack the current row into m_table->record[0], but with
  // a different columns bitmap.
  int unpack_current_row(rpl_group_info *rgi, MY_BITMAP const *cols)
  {
    DBUG_ASSERT(m_table);

    ASSERT_OR_RETURN_ERROR(m_curr_row <= m_rows_end, HA_ERR_CORRUPT_EVENT);
    return ::unpack_row(rgi, m_table, m_width, m_curr_row, cols,
                                   &m_curr_row_end, &m_master_reclength, m_rows_end);
  }

  // Unpack the current row into m_table->record[0]
  int unpack_current_row(rpl_group_info *rgi)
  {
    DBUG_ASSERT(m_table);

    ASSERT_OR_RETURN_ERROR(m_curr_row <= m_rows_end, HA_ERR_CORRUPT_EVENT);
    return ::unpack_row(rgi, m_table, m_width, m_curr_row, &m_cols,
                                   &m_curr_row_end, &m_master_reclength, m_rows_end);
  }
  bool process_triggers(trg_event_type event,
                        trg_action_time_type time_type,
                        bool old_row_is_record1);

  /**
    Helper function to check whether there is an auto increment
    column on the table where the event is to be applied.

    @return true if there is an autoincrement field on the extra
            columns, false otherwise.
   */
  inline bool is_auto_inc_in_extra_columns()
  {
    DBUG_ASSERT(m_table);
    return (m_table->next_number_field &&
            m_table->next_number_field->field_index >= m_width);
  }
#endif

private:

#if defined(MYSQL_SERVER) && defined(HAVE_REPLICATION)
  virtual int do_apply_event(rpl_group_info *rgi);
  virtual int do_update_pos(rpl_group_info *rgi);
  virtual enum_skip_reason do_shall_skip(rpl_group_info *rgi);

  /*
    Primitive to prepare for a sequence of row executions.

    DESCRIPTION

      Before doing a sequence of do_prepare_row() and do_exec_row()
      calls, this member function should be called to prepare for the
      entire sequence. Typically, this member function will allocate
      space for any buffers that are needed for the two member
      functions mentioned above.

    RETURN VALUE

      The member function will return 0 if all went OK, or a non-zero
      error code otherwise.
  */
  virtual 
  int do_before_row_operations(const Slave_reporting_capability *const log) = 0;

  /*
    Primitive to clean up after a sequence of row executions.

    DESCRIPTION
    
      After doing a sequence of do_prepare_row() and do_exec_row(),
      this member function should be called to clean up and release
      any allocated buffers.
      
      The error argument, if non-zero, indicates an error which happened during
      row processing before this function was called. In this case, even if 
      function is successful, it should return the error code given in the argument.
  */
  virtual 
  int do_after_row_operations(const Slave_reporting_capability *const log,
                              int error) = 0;

  /*
    Primitive to do the actual execution necessary for a row.

    DESCRIPTION
      The member function will do the actual execution needed to handle a row.
      The row is located at m_curr_row. When the function returns,
      m_curr_row_end should point at the next row (one byte after the end
      of the current row).    

    RETURN VALUE
      0 if execution succeeded, 1 if execution failed.
      
  */
  virtual int do_exec_row(rpl_group_info *rli) = 0;
#endif /* defined(MYSQL_SERVER) && defined(HAVE_REPLICATION) */

  friend class Old_rows_log_event;
};

/**
  @class Write_rows_log_event

  Log row insertions and updates. The event contain several
  insert/update rows for a table. Note that each event contains only
  rows for one table.

  @section Write_rows_log_event_binary_format Binary Format
*/
class Write_rows_log_event : public Rows_log_event
{
public:
  enum 
  {
    /* Support interface to THD::binlog_prepare_pending_rows_event */
    TYPE_CODE = WRITE_ROWS_EVENT
  };

#if defined(MYSQL_SERVER)
  Write_rows_log_event(THD*, TABLE*, ulong table_id,
                       bool is_transactional);
#endif
#ifdef HAVE_REPLICATION
  Write_rows_log_event(const uchar *buf, uint event_len,
                       const Format_description_log_event *description_event);
#endif
#if defined(MYSQL_SERVER) 
  static bool binlog_row_logging_function(THD *thd, TABLE *table,
                                          bool is_transactional,
                                          const uchar *before_record
                                          __attribute__((unused)),
                                          const uchar *after_record)
  {
    DBUG_ASSERT(!table->versioned(VERS_TRX_ID));
    return thd->binlog_write_row(table, is_transactional, after_record);
  }
#endif

#if defined(MYSQL_SERVER) && defined(HAVE_REPLICATION)
  uint8 get_trg_event_map();
#endif

private:
  virtual Log_event_type get_general_type_code() { return (Log_event_type)TYPE_CODE; }

#ifdef MYSQL_CLIENT
  bool print(FILE *file, PRINT_EVENT_INFO *print_event_info);
#endif

#if defined(MYSQL_SERVER) && defined(HAVE_REPLICATION)
  virtual int do_before_row_operations(const Slave_reporting_capability *const);
  virtual int do_after_row_operations(const Slave_reporting_capability *const,int);
  virtual int do_exec_row(rpl_group_info *);
#endif
};

class Write_rows_compressed_log_event : public Write_rows_log_event
{
public:
#if defined(MYSQL_SERVER)
  Write_rows_compressed_log_event(THD*, TABLE*, ulong table_id,
                       bool is_transactional);
  virtual bool write();
#endif
#ifdef HAVE_REPLICATION
  Write_rows_compressed_log_event(const uchar *buf, uint event_len,
                       const Format_description_log_event *description_event);
#endif
private:
#if defined(MYSQL_CLIENT)
  bool print(FILE *file, PRINT_EVENT_INFO *print_event_info);
#endif
};

/**
  @class Update_rows_log_event

  Log row updates with a before image. The event contain several
  update rows for a table. Note that each event contains only rows for
  one table.

  Also note that the row data consists of pairs of row data: one row
  for the old data and one row for the new data.

  @section Update_rows_log_event_binary_format Binary Format
*/
class Update_rows_log_event : public Rows_log_event
{
public:
  enum 
  {
    /* Support interface to THD::binlog_prepare_pending_rows_event */
    TYPE_CODE = UPDATE_ROWS_EVENT
  };

#ifdef MYSQL_SERVER
  Update_rows_log_event(THD*, TABLE*, ulong table_id,
                        bool is_transactional);

  void init(MY_BITMAP const *cols);
#endif

  virtual ~Update_rows_log_event();

#ifdef HAVE_REPLICATION
  Update_rows_log_event(const uchar *buf, uint event_len,
			const Format_description_log_event *description_event);
#endif

#ifdef MYSQL_SERVER
  static bool binlog_row_logging_function(THD *thd, TABLE *table,
                                          bool is_transactional,
                                          const uchar *before_record,
                                          const uchar *after_record)
  {
    DBUG_ASSERT(!table->versioned(VERS_TRX_ID));
    return thd->binlog_update_row(table, is_transactional,
                                  before_record, after_record);
  }
#endif

  virtual bool is_valid() const
  {
    return Rows_log_event::is_valid() && m_cols_ai.bitmap;
  }

#if defined(MYSQL_SERVER) && defined(HAVE_REPLICATION)
  uint8 get_trg_event_map();
#endif

protected:
  virtual Log_event_type get_general_type_code() { return (Log_event_type)TYPE_CODE; }

#ifdef MYSQL_CLIENT
  bool print(FILE *file, PRINT_EVENT_INFO *print_event_info);
#endif

#if defined(MYSQL_SERVER) && defined(HAVE_REPLICATION)
  virtual int do_before_row_operations(const Slave_reporting_capability *const);
  virtual int do_after_row_operations(const Slave_reporting_capability *const,int);
  virtual int do_exec_row(rpl_group_info *);
#endif /* defined(MYSQL_SERVER) && defined(HAVE_REPLICATION) */
};

class Update_rows_compressed_log_event : public Update_rows_log_event
{
public:
#if defined(MYSQL_SERVER)
  Update_rows_compressed_log_event(THD*, TABLE*, ulong table_id,
                        bool is_transactional);
  virtual bool write();
#endif
#ifdef HAVE_REPLICATION
  Update_rows_compressed_log_event(const uchar *buf, uint event_len,
                       const Format_description_log_event *description_event);
#endif
private:
#if defined(MYSQL_CLIENT)
  bool print(FILE *file, PRINT_EVENT_INFO *print_event_info);
#endif
};

/**
  @class Delete_rows_log_event

  Log row deletions. The event contain several delete rows for a
  table. Note that each event contains only rows for one table.

  RESPONSIBILITIES

    - Act as a container for rows that has been deleted on the master
      and should be deleted on the slave. 

  COLLABORATION

    Row_writer
      Create the event and add rows to the event.
    Row_reader
      Extract the rows from the event.

  @section Delete_rows_log_event_binary_format Binary Format
*/
class Delete_rows_log_event : public Rows_log_event
{
public:
  enum 
  {
    /* Support interface to THD::binlog_prepare_pending_rows_event */
    TYPE_CODE = DELETE_ROWS_EVENT
  };

#ifdef MYSQL_SERVER
  Delete_rows_log_event(THD*, TABLE*, ulong, bool is_transactional);
#endif
#ifdef HAVE_REPLICATION
  Delete_rows_log_event(const uchar *buf, uint event_len,
			const Format_description_log_event *description_event);
#endif
#ifdef MYSQL_SERVER
  static bool binlog_row_logging_function(THD *thd, TABLE *table,
                                          bool is_transactional,
                                          const uchar *before_record,
                                          const uchar *after_record
                                          __attribute__((unused)))
  {
    DBUG_ASSERT(!table->versioned(VERS_TRX_ID));
    return thd->binlog_delete_row(table, is_transactional,
                                  before_record);
  }
#endif

#if defined(MYSQL_SERVER) && defined(HAVE_REPLICATION)
  uint8 get_trg_event_map();
#endif

protected:
  virtual Log_event_type get_general_type_code() { return (Log_event_type)TYPE_CODE; }

#ifdef MYSQL_CLIENT
  bool print(FILE *file, PRINT_EVENT_INFO *print_event_info);
#endif

#if defined(MYSQL_SERVER) && defined(HAVE_REPLICATION)
  virtual int do_before_row_operations(const Slave_reporting_capability *const);
  virtual int do_after_row_operations(const Slave_reporting_capability *const,int);
  virtual int do_exec_row(rpl_group_info *);
#endif
};

class Delete_rows_compressed_log_event : public Delete_rows_log_event
{
public:
#if defined(MYSQL_SERVER)
  Delete_rows_compressed_log_event(THD*, TABLE*, ulong, bool is_transactional);
  virtual bool write();
#endif
#ifdef HAVE_REPLICATION
  Delete_rows_compressed_log_event(const uchar *buf, uint event_len,
                       const Format_description_log_event *description_event);
#endif
private:
#if defined(MYSQL_CLIENT)
  bool print(FILE *file, PRINT_EVENT_INFO *print_event_info);
#endif
};


#include "log_event_old.h"

/**
  @class Incident_log_event

   Class representing an incident, an occurence out of the ordinary,
   that happened on the master.

   The event is used to inform the slave that something out of the
   ordinary happened on the master that might cause the database to be
   in an inconsistent state.

   <table id="IncidentFormat">
   <caption>Incident event format</caption>
   <tr>
     <th>Symbol</th>
     <th>Format</th>
     <th>Description</th>
   </tr>
   <tr>
     <td>INCIDENT</td>
     <td align="right">2</td>
     <td>Incident number as an unsigned integer</td>
   </tr>
   <tr>
     <td>MSGLEN</td>
     <td align="right">1</td>
     <td>Message length as an unsigned integer</td>
   </tr>
   <tr>
     <td>MESSAGE</td>
     <td align="right">MSGLEN</td>
     <td>The message, if present. Not null terminated.</td>
   </tr>
   </table>

  @section Delete_rows_log_event_binary_format Binary Format
*/
class Incident_log_event : public Log_event {
public:
#ifdef MYSQL_SERVER
  Incident_log_event(THD *thd_arg, Incident incident)
    : Log_event(thd_arg, 0, FALSE), m_incident(incident)
  {
    DBUG_ENTER("Incident_log_event::Incident_log_event");
    DBUG_PRINT("enter", ("m_incident: %d", m_incident));
    m_message.str= NULL;                    /* Just as a precaution */
    m_message.length= 0;
    set_direct_logging();
    /* Replicate the incident regardless of @@skip_replication. */
    flags&= ~LOG_EVENT_SKIP_REPLICATION_F;
    DBUG_VOID_RETURN;
  }

  Incident_log_event(THD *thd_arg, Incident incident, const LEX_CSTRING *msg)
    : Log_event(thd_arg, 0, FALSE), m_incident(incident)
  {
    extern PSI_memory_key key_memory_Incident_log_event_message;
    DBUG_ENTER("Incident_log_event::Incident_log_event");
    DBUG_PRINT("enter", ("m_incident: %d", m_incident));
    m_message.length= 0;
    if (!(m_message.str= (char*) my_malloc(key_memory_Incident_log_event_message,
                                           msg->length + 1, MYF(MY_WME))))
    {
      /* Mark this event invalid */
      m_incident= INCIDENT_NONE;
      DBUG_VOID_RETURN;
    }
    strmake(m_message.str, msg->str, msg->length);
    m_message.length= msg->length;
    set_direct_logging();
    /* Replicate the incident regardless of @@skip_replication. */
    flags&= ~LOG_EVENT_SKIP_REPLICATION_F;
    DBUG_VOID_RETURN;
  }
#endif

#ifdef MYSQL_SERVER
  void pack_info(Protocol*);

  virtual bool write_data_header();
  virtual bool write_data_body();
#endif

  Incident_log_event(const uchar *buf, uint event_len,
                     const Format_description_log_event *descr_event);

  virtual ~Incident_log_event();

#ifdef MYSQL_CLIENT
  virtual bool print(FILE *file, PRINT_EVENT_INFO *print_event_info);
#endif

#if defined(MYSQL_SERVER) && defined(HAVE_REPLICATION)
  virtual int do_apply_event(rpl_group_info *rgi);
#endif

  virtual Log_event_type get_type_code() { return INCIDENT_EVENT; }

  virtual bool is_valid() const
  {
    return m_incident > INCIDENT_NONE && m_incident < INCIDENT_COUNT;
  }
  virtual int get_data_size() {
    return INCIDENT_HEADER_LEN + 1 + (uint) m_message.length;
  }

private:
  const char *description() const;

  Incident m_incident;
  LEX_STRING m_message;
};

/**
  @class Ignorable_log_event

  Base class for ignorable log events. Events deriving from
  this class can be safely ignored by slaves that cannot
  recognize them. Newer slaves, will be able to read and
  handle them. This has been designed to be an open-ended
  architecture, so adding new derived events shall not harm
  the old slaves that support ignorable log event mechanism
  (they will just ignore unrecognized ignorable events).

  @note The only thing that makes an event ignorable is that it has
  the LOG_EVENT_IGNORABLE_F flag set.  It is not strictly necessary
  that ignorable event types derive from Ignorable_log_event; they may
  just as well derive from Log_event and pass LOG_EVENT_IGNORABLE_F as
  argument to the Log_event constructor.
**/

class Ignorable_log_event : public Log_event {
public:
  int number;
  const char *description;

#ifndef MYSQL_CLIENT
  Ignorable_log_event(THD *thd_arg)
    :Log_event(thd_arg, LOG_EVENT_IGNORABLE_F, FALSE),
    number(0), description("internal")
  {
    DBUG_ENTER("Ignorable_log_event::Ignorable_log_event");
    DBUG_VOID_RETURN;
  }
#endif

  Ignorable_log_event(const uchar *buf,
                      const Format_description_log_event *descr_event,
                      const char *event_name);
  virtual ~Ignorable_log_event();

#ifndef MYSQL_CLIENT
  void pack_info(Protocol*);
#endif

#ifdef MYSQL_CLIENT
  virtual bool print(FILE *file, PRINT_EVENT_INFO *print_event_info);
#endif

  virtual Log_event_type get_type_code() { return IGNORABLE_LOG_EVENT; }

  virtual bool is_valid() const { return 1; }

  virtual int get_data_size() { return IGNORABLE_HEADER_LEN; }
};

#ifdef MYSQL_CLIENT
bool copy_cache_to_string_wrapped(IO_CACHE *body,
                                  LEX_STRING *to,
                                  bool do_wrap,
                                  const char *delimiter,
                                  bool is_verbose);
bool copy_cache_to_file_wrapped(IO_CACHE *body,
                                FILE *file,
                                bool do_wrap,
                                const char *delimiter,
                                bool is_verbose);
#endif

#ifdef MYSQL_SERVER
/*****************************************************************************

  Heartbeat Log Event class

  Replication event to ensure to slave that master is alive.
  The event is originated by master's dump thread and sent straight to
  slave without being logged. Slave itself does not store it in relay log
  but rather uses a data for immediate checks and throws away the event.

  Two members of the class log_ident and Log_event::log_pos comprise 
  @see the event_coordinates instance. The coordinates that a heartbeat
  instance carries correspond to the last event master has sent from
  its binlog.

 ****************************************************************************/
class Heartbeat_log_event: public Log_event
{
public:
  uint8 hb_flags;
  Heartbeat_log_event(const uchar *buf, uint event_len,
                      const Format_description_log_event* description_event);
  Log_event_type get_type_code() { return HEARTBEAT_LOG_EVENT; }
  bool is_valid() const
    {
      return (log_ident != NULL && ident_len <= FN_REFLEN-1 &&
              log_pos >= BIN_LOG_HEADER_SIZE);
    }
  const uchar * get_log_ident() { return log_ident; }
  uint get_ident_len() { return ident_len; }
  
private:
  uint ident_len;
  const uchar *log_ident;
};

inline int Log_event_writer::write(Log_event *ev)
{
  ev->writer= this;
  int res= ev->write();
  IF_DBUG(ev->writer= 0,); // writer must be set before every Log_event::write
  add_status(ev->logged_status());
  return res;
}

/**
   The function is called by slave applier in case there are
   active table filtering rules to force gathering events associated
   with Query-log-event into an array to execute
   them once the fate of the Query is determined for execution.
*/
bool slave_execute_deferred_events(THD *thd);
#endif

bool event_that_should_be_ignored(const uchar *buf);
bool event_checksum_test(uchar *buf, ulong event_len,
                         enum_binlog_checksum_alg alg);
enum enum_binlog_checksum_alg get_checksum_alg(const uchar *buf, ulong len);
extern TYPELIB binlog_checksum_typelib;
#ifdef WITH_WSREP
enum Log_event_type wsrep_peak_event(rpl_group_info *rgi, ulonglong* event_size);
#endif /* WITH_WSREP */

/**
  @} (end of group Replication)
*/


int binlog_buf_compress(const uchar *src, uchar *dst, uint32 len,
                        uint32 *comlen);
int binlog_buf_uncompress(const uchar *src, uchar *dst, uint32 len,
                          uint32 *newlen);
uint32 binlog_get_compress_len(uint32 len);
uint32 binlog_get_uncompress_len(const uchar *buf);

int query_event_uncompress(const Format_description_log_event *description_event,
                           bool contain_checksum,
                           const uchar *src, ulong src_len, uchar *buf,
                           ulong buf_size, bool* is_malloc,
                           uchar **dst, ulong *newlen);
int row_log_event_uncompress(const Format_description_log_event
                             *description_event,
                             bool contain_checksum,
                             const uchar *src, ulong src_len,
                             uchar* buf, ulong buf_size, bool *is_malloc,
                             uchar **dst, ulong *newlen);

#endif /* _log_event_h */
