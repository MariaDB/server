--
-- PERFORMANCE SCHEMA INSTALLATION
-- Note that this script is also reused by mysql_upgrade,
-- so we have to be very careful here to not destroy any
-- existing database named 'performance_schema' if it
-- can contain user data.
-- In case of downgrade, it's ok to drop unknown tables
-- from a future version, as long as they belong to the
-- performance schema engine.
--

set @have_old_pfs= (select count(*) from information_schema.schemata where schema_name='performance_schema');

SET @cmd="SET @broken_tables = (select count(*) from information_schema.tables  where engine != 'PERFORMANCE_SCHEMA' and table_schema='performance_schema')";

-- Work around for bug#49542
SET @str = IF(@have_old_pfs = 1, @cmd, 'SET @broken_tables = 0');
PREPARE stmt FROM @str;
EXECUTE stmt;
DROP PREPARE stmt;

SET @cmd="SET @broken_views = (select count(*) from information_schema.views where table_schema='performance_schema')";

-- Work around for bug#49542
SET @str = IF(@have_old_pfs = 1, @cmd, 'SET @broken_views = 0');
PREPARE stmt FROM @str;
EXECUTE stmt;
DROP PREPARE stmt;

SET @broken_routines = (select count(*) from mysql.proc where db='performance_schema');

SET @broken_events = (select count(*) from mysql.event where db='performance_schema');

SET @broken_pfs= (select @broken_tables + @broken_views + @broken_routines + @broken_events);

--
-- The performance schema database.
-- Only drop and create the database if this is safe (no broken_pfs).
-- This database is created, even in --without-perfschema builds,
-- so that the database name is always reserved by the MySQL implementation.
--

SET @cmd= "DROP DATABASE IF EXISTS performance_schema";

SET @str = IF(@broken_pfs = 0, @cmd, 'SET @dummy = 0');
PREPARE stmt FROM @str;
EXECUTE stmt;
DROP PREPARE stmt;

SET @cmd= "CREATE DATABASE performance_schema character set utf8";

SET @str = IF(@broken_pfs = 0, @cmd, 'SET @dummy = 0');
PREPARE stmt FROM @str;
EXECUTE stmt;
DROP PREPARE stmt;

--
-- From this point, only create the performance schema tables
-- if the server is built with performance schema
--

set @have_pfs= (select count(engine) from information_schema.engines where engine='PERFORMANCE_SCHEMA' and support != 'NO');

--
-- TABLE COND_INSTANCES
--

SET @cmd="CREATE TABLE performance_schema.cond_instances("
  "NAME VARCHAR(128) not null,"
  "OBJECT_INSTANCE_BEGIN BIGINT unsigned not null"
  ")ENGINE=PERFORMANCE_SCHEMA;";

SET @str = IF(@have_pfs = 1, @cmd, 'SET @dummy = 0');
PREPARE stmt FROM @str;
EXECUTE stmt;
DROP PREPARE stmt;

--
-- TABLE EVENTS_WAITS_CURRENT
--

SET @cmd="CREATE TABLE performance_schema.events_waits_current("
  "THREAD_ID BIGINT unsigned not null,"
  "EVENT_ID BIGINT unsigned not null,"
  "END_EVENT_ID BIGINT unsigned,"
  "EVENT_NAME VARCHAR(128) not null,"
  "SOURCE VARCHAR(64),"
  "TIMER_START BIGINT unsigned,"
  "TIMER_END BIGINT unsigned,"
  "TIMER_WAIT BIGINT unsigned,"
  "SPINS INTEGER unsigned,"
  "OBJECT_SCHEMA VARCHAR(64),"
  "OBJECT_NAME VARCHAR(512),"
  "INDEX_NAME VARCHAR(64),"
  "OBJECT_TYPE VARCHAR(64),"
  "OBJECT_INSTANCE_BEGIN BIGINT unsigned not null,"
  "NESTING_EVENT_ID BIGINT unsigned,"
  "NESTING_EVENT_TYPE ENUM('STATEMENT', 'STAGE', 'WAIT'),"
  "OPERATION VARCHAR(32) not null,"
  "NUMBER_OF_BYTES BIGINT,"
  "FLAGS INTEGER unsigned"
  ")ENGINE=PERFORMANCE_SCHEMA;";

SET @str = IF(@have_pfs = 1, @cmd, 'SET @dummy = 0');
PREPARE stmt FROM @str;
EXECUTE stmt;
DROP PREPARE stmt;

--
-- TABLE EVENTS_WAITS_HISTORY
--

SET @cmd="CREATE TABLE performance_schema.events_waits_history("
  "THREAD_ID BIGINT unsigned not null,"
  "EVENT_ID BIGINT unsigned not null,"
  "END_EVENT_ID BIGINT unsigned,"
  "EVENT_NAME VARCHAR(128) not null,"
  "SOURCE VARCHAR(64),"
  "TIMER_START BIGINT unsigned,"
  "TIMER_END BIGINT unsigned,"
  "TIMER_WAIT BIGINT unsigned,"
  "SPINS INTEGER unsigned,"
  "OBJECT_SCHEMA VARCHAR(64),"
  "OBJECT_NAME VARCHAR(512),"
  "INDEX_NAME VARCHAR(64),"
  "OBJECT_TYPE VARCHAR(64),"
  "OBJECT_INSTANCE_BEGIN BIGINT unsigned not null,"
  "NESTING_EVENT_ID BIGINT unsigned,"
  "NESTING_EVENT_TYPE ENUM('STATEMENT', 'STAGE', 'WAIT'),"
  "OPERATION VARCHAR(32) not null,"
  "NUMBER_OF_BYTES BIGINT,"
  "FLAGS INTEGER unsigned"
  ")ENGINE=PERFORMANCE_SCHEMA;";

SET @str = IF(@have_pfs = 1, @cmd, 'SET @dummy = 0');
PREPARE stmt FROM @str;
EXECUTE stmt;
DROP PREPARE stmt;

--
-- TABLE EVENTS_WAITS_HISTORY_LONG
--

SET @cmd="CREATE TABLE performance_schema.events_waits_history_long("
  "THREAD_ID BIGINT unsigned not null,"
  "EVENT_ID BIGINT unsigned not null,"
  "END_EVENT_ID BIGINT unsigned,"
  "EVENT_NAME VARCHAR(128) not null,"
  "SOURCE VARCHAR(64),"
  "TIMER_START BIGINT unsigned,"
  "TIMER_END BIGINT unsigned,"
  "TIMER_WAIT BIGINT unsigned,"
  "SPINS INTEGER unsigned,"
  "OBJECT_SCHEMA VARCHAR(64),"
  "OBJECT_NAME VARCHAR(512),"
  "INDEX_NAME VARCHAR(64),"
  "OBJECT_TYPE VARCHAR(64),"
  "OBJECT_INSTANCE_BEGIN BIGINT unsigned not null,"
  "NESTING_EVENT_ID BIGINT unsigned,"
  "NESTING_EVENT_TYPE ENUM('STATEMENT', 'STAGE', 'WAIT'),"
  "OPERATION VARCHAR(32) not null,"
  "NUMBER_OF_BYTES BIGINT,"
  "FLAGS INTEGER unsigned"
  ")ENGINE=PERFORMANCE_SCHEMA;";

SET @str = IF(@have_pfs = 1, @cmd, 'SET @dummy = 0');
PREPARE stmt FROM @str;
EXECUTE stmt;
DROP PREPARE stmt;

--
-- TABLE EVENTS_WAITS_SUMMARY_BY_INSTANCE
--

SET @cmd="CREATE TABLE performance_schema.events_waits_summary_by_instance("
  "EVENT_NAME VARCHAR(128) not null,"
  "OBJECT_INSTANCE_BEGIN BIGINT unsigned not null,"
  "COUNT_STAR BIGINT unsigned not null,"
  "SUM_TIMER_WAIT BIGINT unsigned not null,"
  "MIN_TIMER_WAIT BIGINT unsigned not null,"
  "AVG_TIMER_WAIT BIGINT unsigned not null,"
  "MAX_TIMER_WAIT BIGINT unsigned not null"
  ")ENGINE=PERFORMANCE_SCHEMA;";

SET @str = IF(@have_pfs = 1, @cmd, 'SET @dummy = 0');
PREPARE stmt FROM @str;
EXECUTE stmt;
DROP PREPARE stmt;

--
-- TABLE EVENTS_WAITS_SUMMARY_BY_HOST_BY_EVENT_NAME
--

SET @cmd="CREATE TABLE performance_schema.events_waits_summary_by_host_by_event_name("
  "HOST CHAR(60) collate utf8_bin default null,"
  "EVENT_NAME VARCHAR(128) not null,"
  "COUNT_STAR BIGINT unsigned not null,"
  "SUM_TIMER_WAIT BIGINT unsigned not null,"
  "MIN_TIMER_WAIT BIGINT unsigned not null,"
  "AVG_TIMER_WAIT BIGINT unsigned not null,"
  "MAX_TIMER_WAIT BIGINT unsigned not null"
  ")ENGINE=PERFORMANCE_SCHEMA;";

SET @str = IF(@have_pfs = 1, @cmd, 'SET @dummy = 0');
PREPARE stmt FROM @str;
EXECUTE stmt;
DROP PREPARE stmt;

--
-- TABLE EVENTS_WAITS_SUMMARY_BY_USER_BY_EVENT_NAME
--

SET @cmd="CREATE TABLE performance_schema.events_waits_summary_by_user_by_event_name("
  "USER CHAR(16) collate utf8_bin default null,"
  "EVENT_NAME VARCHAR(128) not null,"
  "COUNT_STAR BIGINT unsigned not null,"
  "SUM_TIMER_WAIT BIGINT unsigned not null,"
  "MIN_TIMER_WAIT BIGINT unsigned not null,"
  "AVG_TIMER_WAIT BIGINT unsigned not null,"
  "MAX_TIMER_WAIT BIGINT unsigned not null"
  ")ENGINE=PERFORMANCE_SCHEMA;";

SET @str = IF(@have_pfs = 1, @cmd, 'SET @dummy = 0');
PREPARE stmt FROM @str;
EXECUTE stmt;
DROP PREPARE stmt;

--
-- TABLE EVENTS_WAITS_SUMMARY_BY_ACCOUNT_BY_EVENT_NAME
--

SET @cmd="CREATE TABLE performance_schema.events_waits_summary_by_account_by_event_name("
  "USER CHAR(16) collate utf8_bin default null,"
  "HOST CHAR(60) collate utf8_bin default null,"
  "EVENT_NAME VARCHAR(128) not null,"
  "COUNT_STAR BIGINT unsigned not null,"
  "SUM_TIMER_WAIT BIGINT unsigned not null,"
  "MIN_TIMER_WAIT BIGINT unsigned not null,"
  "AVG_TIMER_WAIT BIGINT unsigned not null,"
  "MAX_TIMER_WAIT BIGINT unsigned not null"
  ")ENGINE=PERFORMANCE_SCHEMA;";

SET @str = IF(@have_pfs = 1, @cmd, 'SET @dummy = 0');
PREPARE stmt FROM @str;
EXECUTE stmt;
DROP PREPARE stmt;

--
-- TABLE EVENTS_WAITS_SUMMARY_BY_THREAD_BY_EVENT_NAME
--

SET @cmd="CREATE TABLE performance_schema.events_waits_summary_by_thread_by_event_name("
  "THREAD_ID BIGINT unsigned not null,"
  "EVENT_NAME VARCHAR(128) not null,"
  "COUNT_STAR BIGINT unsigned not null,"
  "SUM_TIMER_WAIT BIGINT unsigned not null,"
  "MIN_TIMER_WAIT BIGINT unsigned not null,"
  "AVG_TIMER_WAIT BIGINT unsigned not null,"
  "MAX_TIMER_WAIT BIGINT unsigned not null"
  ")ENGINE=PERFORMANCE_SCHEMA;";

SET @str = IF(@have_pfs = 1, @cmd, 'SET @dummy = 0');
PREPARE stmt FROM @str;
EXECUTE stmt;
DROP PREPARE stmt;

--
-- TABLE EVENTS_WAITS_SUMMARY_GLOBAL_BY_EVENT_NAME
--

SET @cmd="CREATE TABLE performance_schema.events_waits_summary_global_by_event_name("
  "EVENT_NAME VARCHAR(128) not null,"
  "COUNT_STAR BIGINT unsigned not null,"
  "SUM_TIMER_WAIT BIGINT unsigned not null,"
  "MIN_TIMER_WAIT BIGINT unsigned not null,"
  "AVG_TIMER_WAIT BIGINT unsigned not null,"
  "MAX_TIMER_WAIT BIGINT unsigned not null"
  ")ENGINE=PERFORMANCE_SCHEMA;";

SET @str = IF(@have_pfs = 1, @cmd, 'SET @dummy = 0');
PREPARE stmt FROM @str;
EXECUTE stmt;
DROP PREPARE stmt;

--
-- TABLE FILE_INSTANCES
--

SET @cmd="CREATE TABLE performance_schema.file_instances("
  "FILE_NAME VARCHAR(512) not null,"
  "EVENT_NAME VARCHAR(128) not null,"
  "OPEN_COUNT INTEGER unsigned not null"
  ")ENGINE=PERFORMANCE_SCHEMA;";

SET @str = IF(@have_pfs = 1, @cmd, 'SET @dummy = 0');
PREPARE stmt FROM @str;
EXECUTE stmt;
DROP PREPARE stmt;

--
-- TABLE FILE_SUMMARY_BY_EVENT_NAME
--

SET @cmd="CREATE TABLE performance_schema.file_summary_by_event_name("
  "EVENT_NAME VARCHAR(128) not null,"
  "COUNT_STAR BIGINT unsigned not null,"
  "SUM_TIMER_WAIT BIGINT unsigned not null,"
  "MIN_TIMER_WAIT BIGINT unsigned not null,"
  "AVG_TIMER_WAIT BIGINT unsigned not null,"
  "MAX_TIMER_WAIT BIGINT unsigned not null,"
  "COUNT_READ BIGINT unsigned not null,"
  "SUM_TIMER_READ BIGINT unsigned not null,"
  "MIN_TIMER_READ BIGINT unsigned not null,"
  "AVG_TIMER_READ BIGINT unsigned not null,"
  "MAX_TIMER_READ BIGINT unsigned not null,"
  "SUM_NUMBER_OF_BYTES_READ BIGINT not null,"
  "COUNT_WRITE BIGINT unsigned not null,"
  "SUM_TIMER_WRITE BIGINT unsigned not null,"
  "MIN_TIMER_WRITE BIGINT unsigned not null,"
  "AVG_TIMER_WRITE BIGINT unsigned not null,"
  "MAX_TIMER_WRITE BIGINT unsigned not null,"
  "SUM_NUMBER_OF_BYTES_WRITE BIGINT not null,"
  "COUNT_MISC BIGINT unsigned not null,"
  "SUM_TIMER_MISC BIGINT unsigned not null,"
  "MIN_TIMER_MISC BIGINT unsigned not null,"
  "AVG_TIMER_MISC BIGINT unsigned not null,"
  "MAX_TIMER_MISC BIGINT unsigned not null"
  ")ENGINE=PERFORMANCE_SCHEMA;";

SET @str = IF(@have_pfs = 1, @cmd, 'SET @dummy = 0');
PREPARE stmt FROM @str;
EXECUTE stmt;
DROP PREPARE stmt;

--
-- TABLE FILE_SUMMARY_BY_INSTANCE
--

SET @cmd="CREATE TABLE performance_schema.file_summary_by_instance("
  "FILE_NAME VARCHAR(512) not null,"
  "EVENT_NAME VARCHAR(128) not null,"
  "OBJECT_INSTANCE_BEGIN BIGINT unsigned not null,"
  "COUNT_STAR BIGINT unsigned not null,"
  "SUM_TIMER_WAIT BIGINT unsigned not null,"
  "MIN_TIMER_WAIT BIGINT unsigned not null,"
  "AVG_TIMER_WAIT BIGINT unsigned not null,"
  "MAX_TIMER_WAIT BIGINT unsigned not null,"
  "COUNT_READ BIGINT unsigned not null,"
  "SUM_TIMER_READ BIGINT unsigned not null,"
  "MIN_TIMER_READ BIGINT unsigned not null,"
  "AVG_TIMER_READ BIGINT unsigned not null,"
  "MAX_TIMER_READ BIGINT unsigned not null,"
  "SUM_NUMBER_OF_BYTES_READ BIGINT not null,"
  "COUNT_WRITE BIGINT unsigned not null,"
  "SUM_TIMER_WRITE BIGINT unsigned not null,"
  "MIN_TIMER_WRITE BIGINT unsigned not null,"
  "AVG_TIMER_WRITE BIGINT unsigned not null,"
  "MAX_TIMER_WRITE BIGINT unsigned not null,"
  "SUM_NUMBER_OF_BYTES_WRITE BIGINT not null,"
  "COUNT_MISC BIGINT unsigned not null,"
  "SUM_TIMER_MISC BIGINT unsigned not null,"
  "MIN_TIMER_MISC BIGINT unsigned not null,"
  "AVG_TIMER_MISC BIGINT unsigned not null,"
  "MAX_TIMER_MISC BIGINT unsigned not null"
  ")ENGINE=PERFORMANCE_SCHEMA;";

SET @str = IF(@have_pfs = 1, @cmd, 'SET @dummy = 0');
PREPARE stmt FROM @str;
EXECUTE stmt;
DROP PREPARE stmt;


--
-- TABLE SOCKET_INSTANCES
--

SET @cmd="CREATE TABLE performance_schema.socket_instances("
  "EVENT_NAME VARCHAR(128) not null,"
  "OBJECT_INSTANCE_BEGIN BIGINT unsigned not null,"
  "THREAD_ID BIGINT unsigned,"
  "SOCKET_ID INTEGER not null,"
  "IP VARCHAR(64) not null,"
  "PORT INTEGER not null,"
  "STATE ENUM('IDLE','ACTIVE') not null"
  ")ENGINE=PERFORMANCE_SCHEMA;";

SET @str = IF(@have_pfs = 1, @cmd, 'SET @dummy = 0');
PREPARE stmt FROM @str;
EXECUTE stmt;
DROP PREPARE stmt;

--
-- TABLE SOCKET_SUMMARY_BY_INSTANCE
--

SET @cmd="CREATE TABLE performance_schema.socket_summary_by_instance("
  "EVENT_NAME VARCHAR(128) not null,"
  "OBJECT_INSTANCE_BEGIN BIGINT unsigned not null,"
  "COUNT_STAR BIGINT unsigned not null,"
  "SUM_TIMER_WAIT BIGINT unsigned not null,"
  "MIN_TIMER_WAIT BIGINT unsigned not null,"
  "AVG_TIMER_WAIT BIGINT unsigned not null,"
  "MAX_TIMER_WAIT BIGINT unsigned not null,"
  "COUNT_READ BIGINT unsigned not null,"
  "SUM_TIMER_READ BIGINT unsigned not null,"
  "MIN_TIMER_READ BIGINT unsigned not null,"
  "AVG_TIMER_READ BIGINT unsigned not null,"
  "MAX_TIMER_READ BIGINT unsigned not null,"
  "SUM_NUMBER_OF_BYTES_READ BIGINT unsigned not null,"
  "COUNT_WRITE BIGINT unsigned not null,"
  "SUM_TIMER_WRITE BIGINT unsigned not null,"
  "MIN_TIMER_WRITE BIGINT unsigned not null,"
  "AVG_TIMER_WRITE BIGINT unsigned not null,"
  "MAX_TIMER_WRITE BIGINT unsigned not null,"
  "SUM_NUMBER_OF_BYTES_WRITE BIGINT unsigned not null,"
  "COUNT_MISC BIGINT unsigned not null,"
  "SUM_TIMER_MISC BIGINT unsigned not null,"
  "MIN_TIMER_MISC BIGINT unsigned not null,"
  "AVG_TIMER_MISC BIGINT unsigned not null,"
  "MAX_TIMER_MISC BIGINT unsigned not null"
  ")ENGINE=PERFORMANCE_SCHEMA;";

SET @str = IF(@have_pfs = 1, @cmd, 'SET @dummy = 0');
PREPARE stmt FROM @str;
EXECUTE stmt;
DROP PREPARE stmt;

--
-- TABLE SOCKET_SUMMARY_BY_INSTANCE
--

SET @cmd="CREATE TABLE performance_schema.socket_summary_by_event_name("
  "EVENT_NAME VARCHAR(128) not null,"
  "COUNT_STAR BIGINT unsigned not null,"
  "SUM_TIMER_WAIT BIGINT unsigned not null,"
  "MIN_TIMER_WAIT BIGINT unsigned not null,"
  "AVG_TIMER_WAIT BIGINT unsigned not null,"
  "MAX_TIMER_WAIT BIGINT unsigned not null,"
  "COUNT_READ BIGINT unsigned not null,"
  "SUM_TIMER_READ BIGINT unsigned not null,"
  "MIN_TIMER_READ BIGINT unsigned not null,"
  "AVG_TIMER_READ BIGINT unsigned not null,"
  "MAX_TIMER_READ BIGINT unsigned not null,"
  "SUM_NUMBER_OF_BYTES_READ BIGINT unsigned not null,"
  "COUNT_WRITE BIGINT unsigned not null,"
  "SUM_TIMER_WRITE BIGINT unsigned not null,"
  "MIN_TIMER_WRITE BIGINT unsigned not null,"
  "AVG_TIMER_WRITE BIGINT unsigned not null,"
  "MAX_TIMER_WRITE BIGINT unsigned not null,"
  "SUM_NUMBER_OF_BYTES_WRITE BIGINT unsigned not null,"
  "COUNT_MISC BIGINT unsigned not null,"
  "SUM_TIMER_MISC BIGINT unsigned not null,"
  "MIN_TIMER_MISC BIGINT unsigned not null,"
  "AVG_TIMER_MISC BIGINT unsigned not null,"
  "MAX_TIMER_MISC BIGINT unsigned not null"
  ")ENGINE=PERFORMANCE_SCHEMA;";

SET @str = IF(@have_pfs = 1, @cmd, 'SET @dummy = 0');
PREPARE stmt FROM @str;
EXECUTE stmt;
DROP PREPARE stmt;

--
-- TABLE HOST_CACHE
--

SET @cmd="CREATE TABLE performance_schema.host_cache("
  "IP VARCHAR(64) not null,"
  "HOST VARCHAR(255) collate utf8_bin,"
  "HOST_VALIDATED ENUM ('YES', 'NO') not null,"
  "SUM_CONNECT_ERRORS BIGINT not null,"
  "COUNT_HOST_BLOCKED_ERRORS BIGINT not null,"
  "COUNT_NAMEINFO_TRANSIENT_ERRORS BIGINT not null,"
  "COUNT_NAMEINFO_PERMANENT_ERRORS BIGINT not null,"
  "COUNT_FORMAT_ERRORS BIGINT not null,"
  "COUNT_ADDRINFO_TRANSIENT_ERRORS BIGINT not null,"
  "COUNT_ADDRINFO_PERMANENT_ERRORS BIGINT not null,"
  "COUNT_FCRDNS_ERRORS BIGINT not null,"
  "COUNT_HOST_ACL_ERRORS BIGINT not null,"
  "COUNT_NO_AUTH_PLUGIN_ERRORS BIGINT not null,"
  "COUNT_AUTH_PLUGIN_ERRORS BIGINT not null,"
  "COUNT_HANDSHAKE_ERRORS BIGINT not null,"
  "COUNT_PROXY_USER_ERRORS BIGINT not null,"
  "COUNT_PROXY_USER_ACL_ERRORS BIGINT not null,"
  "COUNT_AUTHENTICATION_ERRORS BIGINT not null,"
  "COUNT_SSL_ERRORS BIGINT not null,"
  "COUNT_MAX_USER_CONNECTIONS_ERRORS BIGINT not null,"
  "COUNT_MAX_USER_CONNECTIONS_PER_HOUR_ERRORS BIGINT not null,"
  "COUNT_DEFAULT_DATABASE_ERRORS BIGINT not null,"
  "COUNT_INIT_CONNECT_ERRORS BIGINT not null,"
  "COUNT_LOCAL_ERRORS BIGINT not null,"
  "COUNT_UNKNOWN_ERRORS BIGINT not null,"
  "FIRST_SEEN TIMESTAMP(0) NOT NULL default 0,"
  "LAST_SEEN TIMESTAMP(0) NOT NULL default 0,"
  "FIRST_ERROR_SEEN TIMESTAMP(0) null default 0,"
  "LAST_ERROR_SEEN TIMESTAMP(0) null default 0"
  ")ENGINE=PERFORMANCE_SCHEMA;";

SET @str = IF(@have_pfs = 1, @cmd, 'SET @dummy = 0');
PREPARE stmt FROM @str;
EXECUTE stmt;
DROP PREPARE stmt;

--
-- TABLE MUTEX_INSTANCES
--

SET @cmd="CREATE TABLE performance_schema.mutex_instances("
  "NAME VARCHAR(128) not null,"
  "OBJECT_INSTANCE_BEGIN BIGINT unsigned not null,"
  "LOCKED_BY_THREAD_ID BIGINT unsigned"
  ")ENGINE=PERFORMANCE_SCHEMA;";

SET @str = IF(@have_pfs = 1, @cmd, 'SET @dummy = 0');
PREPARE stmt FROM @str;
EXECUTE stmt;
DROP PREPARE stmt;

--
-- TABLE OBJECTS_SUMMARY_GLOBAL_BY_TYPE
--

SET @cmd="CREATE TABLE performance_schema.objects_summary_global_by_type("
  "OBJECT_TYPE VARCHAR(64),"
  "OBJECT_SCHEMA VARCHAR(64),"
  "OBJECT_NAME VARCHAR(64),"
  "COUNT_STAR BIGINT unsigned not null,"
  "SUM_TIMER_WAIT BIGINT unsigned not null,"
  "MIN_TIMER_WAIT BIGINT unsigned not null,"
  "AVG_TIMER_WAIT BIGINT unsigned not null,"
  "MAX_TIMER_WAIT BIGINT unsigned not null"
  ")ENGINE=PERFORMANCE_SCHEMA;";

SET @str = IF(@have_pfs = 1, @cmd, 'SET @dummy = 0');
PREPARE stmt FROM @str;
EXECUTE stmt;
DROP PREPARE stmt;

--
-- TABLE PERFORMANCE_TIMERS
--

SET @cmd="CREATE TABLE performance_schema.performance_timers("
  "TIMER_NAME ENUM ('CYCLE', 'NANOSECOND', 'MICROSECOND', 'MILLISECOND', 'TICK') not null,"
  "TIMER_FREQUENCY BIGINT,"
  "TIMER_RESOLUTION BIGINT,"
  "TIMER_OVERHEAD BIGINT"
  ") ENGINE=PERFORMANCE_SCHEMA;";

SET @str = IF(@have_pfs = 1, @cmd, 'SET @dummy = 0');
PREPARE stmt FROM @str;
EXECUTE stmt;
DROP PREPARE stmt;

--
-- TABLE RWLOCK_INSTANCES
--

SET @cmd="CREATE TABLE performance_schema.rwlock_instances("
  "NAME VARCHAR(128) not null,"
  "OBJECT_INSTANCE_BEGIN BIGINT unsigned not null,"
  "WRITE_LOCKED_BY_THREAD_ID BIGINT unsigned,"
  "READ_LOCKED_BY_COUNT INTEGER unsigned not null"
  ")ENGINE=PERFORMANCE_SCHEMA;";

SET @str = IF(@have_pfs = 1, @cmd, 'SET @dummy = 0');
PREPARE stmt FROM @str;
EXECUTE stmt;
DROP PREPARE stmt;

--
-- TABLE SETUP_ACTORS
--

SET @cmd="CREATE TABLE performance_schema.setup_actors("
  "HOST CHAR(60) collate utf8_bin default '%' not null,"
  "USER CHAR(16) collate utf8_bin default '%' not null,"
  "ROLE CHAR(16) collate utf8_bin default '%' not null"
  ")ENGINE=PERFORMANCE_SCHEMA;";

SET @str = IF(@have_pfs = 1, @cmd, 'SET @dummy = 0');
PREPARE stmt FROM @str;
EXECUTE stmt;
DROP PREPARE stmt;

--
-- TABLE SETUP_CONSUMERS
--

SET @cmd="CREATE TABLE performance_schema.setup_consumers("
  "NAME VARCHAR(64) not null,"
  "ENABLED ENUM ('YES', 'NO') not null"
  ")ENGINE=PERFORMANCE_SCHEMA;";

SET @str = IF(@have_pfs = 1, @cmd, 'SET @dummy = 0');
PREPARE stmt FROM @str;
EXECUTE stmt;
DROP PREPARE stmt;

--
-- TABLE SETUP_INSTRUMENTS
--

SET @cmd="CREATE TABLE performance_schema.setup_instruments("
  "NAME VARCHAR(128) not null,"
  "ENABLED ENUM ('YES', 'NO') not null,"
  "TIMED ENUM ('YES', 'NO') not null"
  ")ENGINE=PERFORMANCE_SCHEMA;";

SET @str = IF(@have_pfs = 1, @cmd, 'SET @dummy = 0');
PREPARE stmt FROM @str;
EXECUTE stmt;
DROP PREPARE stmt;

--
-- TABLE SETUP_OBJECTS
--

SET @cmd="CREATE TABLE performance_schema.setup_objects("
  "OBJECT_TYPE ENUM ('TABLE') not null default 'TABLE',"
  "OBJECT_SCHEMA VARCHAR(64) default '%',"
  "OBJECT_NAME VARCHAR(64) not null default '%',"
  "ENABLED ENUM ('YES', 'NO') not null default 'YES',"
  "TIMED ENUM ('YES', 'NO') not null default 'YES'"
  ")ENGINE=PERFORMANCE_SCHEMA;";

SET @str = IF(@have_pfs = 1, @cmd, 'SET @dummy = 0');
PREPARE stmt FROM @str;
EXECUTE stmt;
DROP PREPARE stmt;

--
-- TABLE SETUP_TIMERS
--

SET @cmd="CREATE TABLE performance_schema.setup_timers("
  "NAME VARCHAR(64) not null,"
  "TIMER_NAME ENUM ('CYCLE', 'NANOSECOND', 'MICROSECOND', 'MILLISECOND', 'TICK') not null"
  ")ENGINE=PERFORMANCE_SCHEMA;";

SET @str = IF(@have_pfs = 1, @cmd, 'SET @dummy = 0');
PREPARE stmt FROM @str;
EXECUTE stmt;
DROP PREPARE stmt;

--
-- TABLE TABLE_IO_WAITS_SUMMARY_BY_INDEX_USAGE
--

SET @cmd="CREATE TABLE performance_schema.table_io_waits_summary_by_index_usage("
  "OBJECT_TYPE VARCHAR(64),"
  "OBJECT_SCHEMA VARCHAR(64),"
  "OBJECT_NAME VARCHAR(64),"
  "INDEX_NAME VARCHAR(64),"
  "COUNT_STAR BIGINT unsigned not null,"
  "SUM_TIMER_WAIT BIGINT unsigned not null,"
  "MIN_TIMER_WAIT BIGINT unsigned not null,"
  "AVG_TIMER_WAIT BIGINT unsigned not null,"
  "MAX_TIMER_WAIT BIGINT unsigned not null,"
  "COUNT_READ BIGINT unsigned not null,"
  "SUM_TIMER_READ BIGINT unsigned not null,"
  "MIN_TIMER_READ BIGINT unsigned not null,"
  "AVG_TIMER_READ BIGINT unsigned not null,"
  "MAX_TIMER_READ BIGINT unsigned not null,"
  "COUNT_WRITE BIGINT unsigned not null,"
  "SUM_TIMER_WRITE BIGINT unsigned not null,"
  "MIN_TIMER_WRITE BIGINT unsigned not null,"
  "AVG_TIMER_WRITE BIGINT unsigned not null,"
  "MAX_TIMER_WRITE BIGINT unsigned not null,"
  "COUNT_FETCH BIGINT unsigned not null,"
  "SUM_TIMER_FETCH BIGINT unsigned not null,"
  "MIN_TIMER_FETCH BIGINT unsigned not null,"
  "AVG_TIMER_FETCH BIGINT unsigned not null,"
  "MAX_TIMER_FETCH BIGINT unsigned not null,"
  "COUNT_INSERT BIGINT unsigned not null,"
  "SUM_TIMER_INSERT BIGINT unsigned not null,"
  "MIN_TIMER_INSERT BIGINT unsigned not null,"
  "AVG_TIMER_INSERT BIGINT unsigned not null,"
  "MAX_TIMER_INSERT BIGINT unsigned not null,"
  "COUNT_UPDATE BIGINT unsigned not null,"
  "SUM_TIMER_UPDATE BIGINT unsigned not null,"
  "MIN_TIMER_UPDATE BIGINT unsigned not null,"
  "AVG_TIMER_UPDATE BIGINT unsigned not null,"
  "MAX_TIMER_UPDATE BIGINT unsigned not null,"
  "COUNT_DELETE BIGINT unsigned not null,"
  "SUM_TIMER_DELETE BIGINT unsigned not null,"
  "MIN_TIMER_DELETE BIGINT unsigned not null,"
  "AVG_TIMER_DELETE BIGINT unsigned not null,"
  "MAX_TIMER_DELETE BIGINT unsigned not null"
  ")ENGINE=PERFORMANCE_SCHEMA;";

SET @str = IF(@have_pfs = 1, @cmd, 'SET @dummy = 0');
PREPARE stmt FROM @str;
EXECUTE stmt;
DROP PREPARE stmt;

--
-- TABLE TABLE_IO_WAITS_SUMMARY_BY_TABLE
--

SET @cmd="CREATE TABLE performance_schema.table_io_waits_summary_by_table("
  "OBJECT_TYPE VARCHAR(64),"
  "OBJECT_SCHEMA VARCHAR(64),"
  "OBJECT_NAME VARCHAR(64),"
  "COUNT_STAR BIGINT unsigned not null,"
  "SUM_TIMER_WAIT BIGINT unsigned not null,"
  "MIN_TIMER_WAIT BIGINT unsigned not null,"
  "AVG_TIMER_WAIT BIGINT unsigned not null,"
  "MAX_TIMER_WAIT BIGINT unsigned not null,"
  "COUNT_READ BIGINT unsigned not null,"
  "SUM_TIMER_READ BIGINT unsigned not null,"
  "MIN_TIMER_READ BIGINT unsigned not null,"
  "AVG_TIMER_READ BIGINT unsigned not null,"
  "MAX_TIMER_READ BIGINT unsigned not null,"
  "COUNT_WRITE BIGINT unsigned not null,"
  "SUM_TIMER_WRITE BIGINT unsigned not null,"
  "MIN_TIMER_WRITE BIGINT unsigned not null,"
  "AVG_TIMER_WRITE BIGINT unsigned not null,"
  "MAX_TIMER_WRITE BIGINT unsigned not null,"
  "COUNT_FETCH BIGINT unsigned not null,"
  "SUM_TIMER_FETCH BIGINT unsigned not null,"
  "MIN_TIMER_FETCH BIGINT unsigned not null,"
  "AVG_TIMER_FETCH BIGINT unsigned not null,"
  "MAX_TIMER_FETCH BIGINT unsigned not null,"
  "COUNT_INSERT BIGINT unsigned not null,"
  "SUM_TIMER_INSERT BIGINT unsigned not null,"
  "MIN_TIMER_INSERT BIGINT unsigned not null,"
  "AVG_TIMER_INSERT BIGINT unsigned not null,"
  "MAX_TIMER_INSERT BIGINT unsigned not null,"
  "COUNT_UPDATE BIGINT unsigned not null,"
  "SUM_TIMER_UPDATE BIGINT unsigned not null,"
  "MIN_TIMER_UPDATE BIGINT unsigned not null,"
  "AVG_TIMER_UPDATE BIGINT unsigned not null,"
  "MAX_TIMER_UPDATE BIGINT unsigned not null,"
  "COUNT_DELETE BIGINT unsigned not null,"
  "SUM_TIMER_DELETE BIGINT unsigned not null,"
  "MIN_TIMER_DELETE BIGINT unsigned not null,"
  "AVG_TIMER_DELETE BIGINT unsigned not null,"
  "MAX_TIMER_DELETE BIGINT unsigned not null"
  ")ENGINE=PERFORMANCE_SCHEMA;";

SET @str = IF(@have_pfs = 1, @cmd, 'SET @dummy = 0');
PREPARE stmt FROM @str;
EXECUTE stmt;
DROP PREPARE stmt;

--
-- TABLE TABLE_LOCK_WAITS_SUMMARY_BY_TABLE
--

SET @cmd="CREATE TABLE performance_schema.table_lock_waits_summary_by_table("
  "OBJECT_TYPE VARCHAR(64),"
  "OBJECT_SCHEMA VARCHAR(64),"
  "OBJECT_NAME VARCHAR(64),"
  "COUNT_STAR BIGINT unsigned not null,"
  "SUM_TIMER_WAIT BIGINT unsigned not null,"
  "MIN_TIMER_WAIT BIGINT unsigned not null,"
  "AVG_TIMER_WAIT BIGINT unsigned not null,"
  "MAX_TIMER_WAIT BIGINT unsigned not null,"
  "COUNT_READ BIGINT unsigned not null,"
  "SUM_TIMER_READ BIGINT unsigned not null,"
  "MIN_TIMER_READ BIGINT unsigned not null,"
  "AVG_TIMER_READ BIGINT unsigned not null,"
  "MAX_TIMER_READ BIGINT unsigned not null,"
  "COUNT_WRITE BIGINT unsigned not null,"
  "SUM_TIMER_WRITE BIGINT unsigned not null,"
  "MIN_TIMER_WRITE BIGINT unsigned not null,"
  "AVG_TIMER_WRITE BIGINT unsigned not null,"
  "MAX_TIMER_WRITE BIGINT unsigned not null,"
  "COUNT_READ_NORMAL BIGINT unsigned not null,"
  "SUM_TIMER_READ_NORMAL BIGINT unsigned not null,"
  "MIN_TIMER_READ_NORMAL BIGINT unsigned not null,"
  "AVG_TIMER_READ_NORMAL BIGINT unsigned not null,"
  "MAX_TIMER_READ_NORMAL BIGINT unsigned not null,"
  "COUNT_READ_WITH_SHARED_LOCKS BIGINT unsigned not null,"
  "SUM_TIMER_READ_WITH_SHARED_LOCKS BIGINT unsigned not null,"
  "MIN_TIMER_READ_WITH_SHARED_LOCKS BIGINT unsigned not null,"
  "AVG_TIMER_READ_WITH_SHARED_LOCKS BIGINT unsigned not null,"
  "MAX_TIMER_READ_WITH_SHARED_LOCKS BIGINT unsigned not null,"
  "COUNT_READ_HIGH_PRIORITY BIGINT unsigned not null,"
  "SUM_TIMER_READ_HIGH_PRIORITY BIGINT unsigned not null,"
  "MIN_TIMER_READ_HIGH_PRIORITY BIGINT unsigned not null,"
  "AVG_TIMER_READ_HIGH_PRIORITY BIGINT unsigned not null,"
  "MAX_TIMER_READ_HIGH_PRIORITY BIGINT unsigned not null,"
  "COUNT_READ_NO_INSERT BIGINT unsigned not null,"
  "SUM_TIMER_READ_NO_INSERT BIGINT unsigned not null,"
  "MIN_TIMER_READ_NO_INSERT BIGINT unsigned not null,"
  "AVG_TIMER_READ_NO_INSERT BIGINT unsigned not null,"
  "MAX_TIMER_READ_NO_INSERT BIGINT unsigned not null,"
  "COUNT_READ_EXTERNAL BIGINT unsigned not null,"
  "SUM_TIMER_READ_EXTERNAL BIGINT unsigned not null,"
  "MIN_TIMER_READ_EXTERNAL BIGINT unsigned not null,"
  "AVG_TIMER_READ_EXTERNAL BIGINT unsigned not null,"
  "MAX_TIMER_READ_EXTERNAL BIGINT unsigned not null,"
  "COUNT_WRITE_ALLOW_WRITE BIGINT unsigned not null,"
  "SUM_TIMER_WRITE_ALLOW_WRITE BIGINT unsigned not null,"
  "MIN_TIMER_WRITE_ALLOW_WRITE BIGINT unsigned not null,"
  "AVG_TIMER_WRITE_ALLOW_WRITE BIGINT unsigned not null,"
  "MAX_TIMER_WRITE_ALLOW_WRITE BIGINT unsigned not null,"
  "COUNT_WRITE_CONCURRENT_INSERT BIGINT unsigned not null,"
  "SUM_TIMER_WRITE_CONCURRENT_INSERT BIGINT unsigned not null,"
  "MIN_TIMER_WRITE_CONCURRENT_INSERT BIGINT unsigned not null,"
  "AVG_TIMER_WRITE_CONCURRENT_INSERT BIGINT unsigned not null,"
  "MAX_TIMER_WRITE_CONCURRENT_INSERT BIGINT unsigned not null,"
  "COUNT_WRITE_DELAYED BIGINT unsigned not null,"
  "SUM_TIMER_WRITE_DELAYED BIGINT unsigned not null,"
  "MIN_TIMER_WRITE_DELAYED BIGINT unsigned not null,"
  "AVG_TIMER_WRITE_DELAYED BIGINT unsigned not null,"
  "MAX_TIMER_WRITE_DELAYED BIGINT unsigned not null,"
  "COUNT_WRITE_LOW_PRIORITY BIGINT unsigned not null,"
  "SUM_TIMER_WRITE_LOW_PRIORITY BIGINT unsigned not null,"
  "MIN_TIMER_WRITE_LOW_PRIORITY BIGINT unsigned not null,"
  "AVG_TIMER_WRITE_LOW_PRIORITY BIGINT unsigned not null,"
  "MAX_TIMER_WRITE_LOW_PRIORITY BIGINT unsigned not null,"
  "COUNT_WRITE_NORMAL BIGINT unsigned not null,"
  "SUM_TIMER_WRITE_NORMAL BIGINT unsigned not null,"
  "MIN_TIMER_WRITE_NORMAL BIGINT unsigned not null,"
  "AVG_TIMER_WRITE_NORMAL BIGINT unsigned not null,"
  "MAX_TIMER_WRITE_NORMAL BIGINT unsigned not null,"
  "COUNT_WRITE_EXTERNAL BIGINT unsigned not null,"
  "SUM_TIMER_WRITE_EXTERNAL BIGINT unsigned not null,"
  "MIN_TIMER_WRITE_EXTERNAL BIGINT unsigned not null,"
  "AVG_TIMER_WRITE_EXTERNAL BIGINT unsigned not null,"
  "MAX_TIMER_WRITE_EXTERNAL BIGINT unsigned not null"
  ")ENGINE=PERFORMANCE_SCHEMA;";

SET @str = IF(@have_pfs = 1, @cmd, 'SET @dummy = 0');
PREPARE stmt FROM @str;
EXECUTE stmt;
DROP PREPARE stmt;

--
-- TABLE THREADS
--

SET @cmd="CREATE TABLE performance_schema.threads("
  "THREAD_ID BIGINT unsigned not null,"
  "NAME VARCHAR(128) not null,"
  "TYPE VARCHAR(10) not null,"
  "PROCESSLIST_ID BIGINT unsigned,"
  "PROCESSLIST_USER VARCHAR(16),"
  "PROCESSLIST_HOST VARCHAR(60),"
  "PROCESSLIST_DB VARCHAR(64),"
  "PROCESSLIST_COMMAND VARCHAR(16),"
  "PROCESSLIST_TIME BIGINT,"
  "PROCESSLIST_STATE VARCHAR(64),"
  "PROCESSLIST_INFO LONGTEXT,"
  "PARENT_THREAD_ID BIGINT unsigned,"
  "ROLE VARCHAR(64),"
  "INSTRUMENTED ENUM ('YES', 'NO') not null"
  ")ENGINE=PERFORMANCE_SCHEMA;";

SET @str = IF(@have_pfs = 1, @cmd, 'SET @dummy = 0');
PREPARE stmt FROM @str;
EXECUTE stmt;
DROP PREPARE stmt;

--
-- TABLE EVENTS_STAGES_CURRENT
--

SET @cmd="CREATE TABLE performance_schema.events_stages_current("
  "THREAD_ID BIGINT unsigned not null,"
  "EVENT_ID BIGINT unsigned not null,"
  "END_EVENT_ID BIGINT unsigned,"
  "EVENT_NAME VARCHAR(128) not null,"
  "SOURCE VARCHAR(64),"
  "TIMER_START BIGINT unsigned,"
  "TIMER_END BIGINT unsigned,"
  "TIMER_WAIT BIGINT unsigned,"
  "NESTING_EVENT_ID BIGINT unsigned,"
  "NESTING_EVENT_TYPE ENUM('STATEMENT', 'STAGE', 'WAIT')"
  ")ENGINE=PERFORMANCE_SCHEMA;";

SET @str = IF(@have_pfs = 1, @cmd, 'SET @dummy = 0');
PREPARE stmt FROM @str;
EXECUTE stmt;
DROP PREPARE stmt;

--
-- TABLE EVENTS_STAGES_HISTORY
--

SET @cmd="CREATE TABLE performance_schema.events_stages_history("
  "THREAD_ID BIGINT unsigned not null,"
  "EVENT_ID BIGINT unsigned not null,"
  "END_EVENT_ID BIGINT unsigned,"
  "EVENT_NAME VARCHAR(128) not null,"
  "SOURCE VARCHAR(64),"
  "TIMER_START BIGINT unsigned,"
  "TIMER_END BIGINT unsigned,"
  "TIMER_WAIT BIGINT unsigned,"
  "NESTING_EVENT_ID BIGINT unsigned,"
  "NESTING_EVENT_TYPE ENUM('STATEMENT', 'STAGE', 'WAIT')"
  ")ENGINE=PERFORMANCE_SCHEMA;";

SET @str = IF(@have_pfs = 1, @cmd, 'SET @dummy = 0');
PREPARE stmt FROM @str;
EXECUTE stmt;
DROP PREPARE stmt;

--
-- TABLE EVENTS_STAGES_HISTORY_LONG
--

SET @cmd="CREATE TABLE performance_schema.events_stages_history_long("
  "THREAD_ID BIGINT unsigned not null,"
  "EVENT_ID BIGINT unsigned not null,"
  "END_EVENT_ID BIGINT unsigned,"
  "EVENT_NAME VARCHAR(128) not null,"
  "SOURCE VARCHAR(64),"
  "TIMER_START BIGINT unsigned,"
  "TIMER_END BIGINT unsigned,"
  "TIMER_WAIT BIGINT unsigned,"
  "NESTING_EVENT_ID BIGINT unsigned,"
  "NESTING_EVENT_TYPE ENUM('STATEMENT', 'STAGE', 'WAIT')"
  ")ENGINE=PERFORMANCE_SCHEMA;";

SET @str = IF(@have_pfs = 1, @cmd, 'SET @dummy = 0');
PREPARE stmt FROM @str;
EXECUTE stmt;
DROP PREPARE stmt;

--
-- TABLE EVENTS_STAGES_SUMMARY_BY_THREAD_BY_EVENT_NAME
--

SET @cmd="CREATE TABLE performance_schema.events_stages_summary_by_thread_by_event_name("
  "THREAD_ID BIGINT unsigned not null,"
  "EVENT_NAME VARCHAR(128) not null,"
  "COUNT_STAR BIGINT unsigned not null,"
  "SUM_TIMER_WAIT BIGINT unsigned not null,"
  "MIN_TIMER_WAIT BIGINT unsigned not null,"
  "AVG_TIMER_WAIT BIGINT unsigned not null,"
  "MAX_TIMER_WAIT BIGINT unsigned not null"
  ")ENGINE=PERFORMANCE_SCHEMA;";

SET @str = IF(@have_pfs = 1, @cmd, 'SET @dummy = 0');
PREPARE stmt FROM @str;
EXECUTE stmt;
DROP PREPARE stmt;

--
-- TABLE EVENTS_STAGES_SUMMARY_BY_HOST_BY_EVENT_NAME
--

SET @cmd="CREATE TABLE performance_schema.events_stages_summary_by_host_by_event_name("
  "HOST CHAR(60) collate utf8_bin default null,"
  "EVENT_NAME VARCHAR(128) not null,"
  "COUNT_STAR BIGINT unsigned not null,"
  "SUM_TIMER_WAIT BIGINT unsigned not null,"
  "MIN_TIMER_WAIT BIGINT unsigned not null,"
  "AVG_TIMER_WAIT BIGINT unsigned not null,"
  "MAX_TIMER_WAIT BIGINT unsigned not null"
  ")ENGINE=PERFORMANCE_SCHEMA;";

SET @str = IF(@have_pfs = 1, @cmd, 'SET @dummy = 0');
PREPARE stmt FROM @str;
EXECUTE stmt;
DROP PREPARE stmt;

--
-- TABLE EVENTS_STAGES_SUMMARY_BY_USER_BY_EVENT_NAME
--

SET @cmd="CREATE TABLE performance_schema.events_stages_summary_by_user_by_event_name("
  "USER CHAR(16) collate utf8_bin default null,"
  "EVENT_NAME VARCHAR(128) not null,"
  "COUNT_STAR BIGINT unsigned not null,"
  "SUM_TIMER_WAIT BIGINT unsigned not null,"
  "MIN_TIMER_WAIT BIGINT unsigned not null,"
  "AVG_TIMER_WAIT BIGINT unsigned not null,"
  "MAX_TIMER_WAIT BIGINT unsigned not null"
  ")ENGINE=PERFORMANCE_SCHEMA;";

SET @str = IF(@have_pfs = 1, @cmd, 'SET @dummy = 0');
PREPARE stmt FROM @str;
EXECUTE stmt;
DROP PREPARE stmt;

--
-- TABLE EVENTS_STAGES_SUMMARY_BY_ACCOUNT_BY_EVENT_NAME
--

SET @cmd="CREATE TABLE performance_schema.events_stages_summary_by_account_by_event_name("
  "USER CHAR(16) collate utf8_bin default null,"
  "HOST CHAR(60) collate utf8_bin default null,"
  "EVENT_NAME VARCHAR(128) not null,"
  "COUNT_STAR BIGINT unsigned not null,"
  "SUM_TIMER_WAIT BIGINT unsigned not null,"
  "MIN_TIMER_WAIT BIGINT unsigned not null,"
  "AVG_TIMER_WAIT BIGINT unsigned not null,"
  "MAX_TIMER_WAIT BIGINT unsigned not null"
  ")ENGINE=PERFORMANCE_SCHEMA;";

SET @str = IF(@have_pfs = 1, @cmd, 'SET @dummy = 0');
PREPARE stmt FROM @str;
EXECUTE stmt;
DROP PREPARE stmt;

--
-- TABLE EVENTS_STAGES_SUMMARY_GLOBAL_BY_EVENT_NAME
--

SET @cmd="CREATE TABLE performance_schema.events_stages_summary_global_by_event_name("
  "EVENT_NAME VARCHAR(128) not null,"
  "COUNT_STAR BIGINT unsigned not null,"
  "SUM_TIMER_WAIT BIGINT unsigned not null,"
  "MIN_TIMER_WAIT BIGINT unsigned not null,"
  "AVG_TIMER_WAIT BIGINT unsigned not null,"
  "MAX_TIMER_WAIT BIGINT unsigned not null"
  ")ENGINE=PERFORMANCE_SCHEMA;";

SET @str = IF(@have_pfs = 1, @cmd, 'SET @dummy = 0');
PREPARE stmt FROM @str;
EXECUTE stmt;
DROP PREPARE stmt;

--
-- TABLE EVENTS_STATEMENTS_CURRENT
--

SET @cmd="CREATE TABLE performance_schema.events_statements_current("
  "THREAD_ID BIGINT unsigned not null,"
  "EVENT_ID BIGINT unsigned not null,"
  "END_EVENT_ID BIGINT unsigned,"
  "EVENT_NAME VARCHAR(128) not null,"
  "SOURCE VARCHAR(64),"
  "TIMER_START BIGINT unsigned,"
  "TIMER_END BIGINT unsigned,"
  "TIMER_WAIT BIGINT unsigned,"
  "LOCK_TIME bigint unsigned not null,"
  "SQL_TEXT LONGTEXT,"
  "DIGEST VARCHAR(32),"
  "DIGEST_TEXT LONGTEXT,"
  "CURRENT_SCHEMA VARCHAR(64),"
  "OBJECT_TYPE VARCHAR(64),"
  "OBJECT_SCHEMA VARCHAR(64),"
  "OBJECT_NAME VARCHAR(64),"
  "OBJECT_INSTANCE_BEGIN BIGINT unsigned,"
  "MYSQL_ERRNO INTEGER,"
  "RETURNED_SQLSTATE VARCHAR(5),"
  "MESSAGE_TEXT VARCHAR(128),"
  "ERRORS BIGINT unsigned not null,"
  "WARNINGS BIGINT unsigned not null,"
  "ROWS_AFFECTED BIGINT unsigned not null,"
  "ROWS_SENT BIGINT unsigned not null,"
  "ROWS_EXAMINED BIGINT unsigned not null,"
  "CREATED_TMP_DISK_TABLES BIGINT unsigned not null,"
  "CREATED_TMP_TABLES BIGINT unsigned not null,"
  "SELECT_FULL_JOIN BIGINT unsigned not null,"
  "SELECT_FULL_RANGE_JOIN BIGINT unsigned not null,"
  "SELECT_RANGE BIGINT unsigned not null,"
  "SELECT_RANGE_CHECK BIGINT unsigned not null,"
  "SELECT_SCAN BIGINT unsigned not null,"
  "SORT_MERGE_PASSES BIGINT unsigned not null,"
  "SORT_RANGE BIGINT unsigned not null,"
  "SORT_ROWS BIGINT unsigned not null,"
  "SORT_SCAN BIGINT unsigned not null,"
  "NO_INDEX_USED BIGINT unsigned not null,"
  "NO_GOOD_INDEX_USED BIGINT unsigned not null,"
  "NESTING_EVENT_ID BIGINT unsigned,"
  "NESTING_EVENT_TYPE ENUM('STATEMENT', 'STAGE', 'WAIT')"
  ")ENGINE=PERFORMANCE_SCHEMA;";

SET @str = IF(@have_pfs = 1, @cmd, 'SET @dummy = 0');
PREPARE stmt FROM @str;
EXECUTE stmt;
DROP PREPARE stmt;

--
-- TABLE EVENTS_STATEMENTS_HISTORY
--

SET @cmd="CREATE TABLE performance_schema.events_statements_history("
  "THREAD_ID BIGINT unsigned not null,"
  "EVENT_ID BIGINT unsigned not null,"
  "END_EVENT_ID BIGINT unsigned,"
  "EVENT_NAME VARCHAR(128) not null,"
  "SOURCE VARCHAR(64),"
  "TIMER_START BIGINT unsigned,"
  "TIMER_END BIGINT unsigned,"
  "TIMER_WAIT BIGINT unsigned,"
  "LOCK_TIME bigint unsigned not null,"
  "SQL_TEXT LONGTEXT,"
  "DIGEST VARCHAR(32),"
  "DIGEST_TEXT LONGTEXT,"
  "CURRENT_SCHEMA VARCHAR(64),"
  "OBJECT_TYPE VARCHAR(64),"
  "OBJECT_SCHEMA VARCHAR(64),"
  "OBJECT_NAME VARCHAR(64),"
  "OBJECT_INSTANCE_BEGIN BIGINT unsigned,"
  "MYSQL_ERRNO INTEGER,"
  "RETURNED_SQLSTATE VARCHAR(5),"
  "MESSAGE_TEXT VARCHAR(128),"
  "ERRORS BIGINT unsigned not null,"
  "WARNINGS BIGINT unsigned not null,"
  "ROWS_AFFECTED BIGINT unsigned not null,"
  "ROWS_SENT BIGINT unsigned not null,"
  "ROWS_EXAMINED BIGINT unsigned not null,"
  "CREATED_TMP_DISK_TABLES BIGINT unsigned not null,"
  "CREATED_TMP_TABLES BIGINT unsigned not null,"
  "SELECT_FULL_JOIN BIGINT unsigned not null,"
  "SELECT_FULL_RANGE_JOIN BIGINT unsigned not null,"
  "SELECT_RANGE BIGINT unsigned not null,"
  "SELECT_RANGE_CHECK BIGINT unsigned not null,"
  "SELECT_SCAN BIGINT unsigned not null,"
  "SORT_MERGE_PASSES BIGINT unsigned not null,"
  "SORT_RANGE BIGINT unsigned not null,"
  "SORT_ROWS BIGINT unsigned not null,"
  "SORT_SCAN BIGINT unsigned not null,"
  "NO_INDEX_USED BIGINT unsigned not null,"
  "NO_GOOD_INDEX_USED BIGINT unsigned not null,"
  "NESTING_EVENT_ID BIGINT unsigned,"
  "NESTING_EVENT_TYPE ENUM('STATEMENT', 'STAGE', 'WAIT')"
  ")ENGINE=PERFORMANCE_SCHEMA;";

SET @str = IF(@have_pfs = 1, @cmd, 'SET @dummy = 0');
PREPARE stmt FROM @str;
EXECUTE stmt;
DROP PREPARE stmt;

--
-- TABLE EVENTS_STATEMENTS_HISTORY_LONG
--

SET @cmd="CREATE TABLE performance_schema.events_statements_history_long("
  "THREAD_ID BIGINT unsigned not null,"
  "EVENT_ID BIGINT unsigned not null,"
  "END_EVENT_ID BIGINT unsigned,"
  "EVENT_NAME VARCHAR(128) not null,"
  "SOURCE VARCHAR(64),"
  "TIMER_START BIGINT unsigned,"
  "TIMER_END BIGINT unsigned,"
  "TIMER_WAIT BIGINT unsigned,"
  "LOCK_TIME bigint unsigned not null,"
  "SQL_TEXT LONGTEXT,"
  "DIGEST VARCHAR(32),"
  "DIGEST_TEXT LONGTEXT,"
  "CURRENT_SCHEMA VARCHAR(64),"
  "OBJECT_TYPE VARCHAR(64),"
  "OBJECT_SCHEMA VARCHAR(64),"
  "OBJECT_NAME VARCHAR(64),"
  "OBJECT_INSTANCE_BEGIN BIGINT unsigned,"
  "MYSQL_ERRNO INTEGER,"
  "RETURNED_SQLSTATE VARCHAR(5),"
  "MESSAGE_TEXT VARCHAR(128),"
  "ERRORS BIGINT unsigned not null,"
  "WARNINGS BIGINT unsigned not null,"
  "ROWS_AFFECTED BIGINT unsigned not null,"
  "ROWS_SENT BIGINT unsigned not null,"
  "ROWS_EXAMINED BIGINT unsigned not null,"
  "CREATED_TMP_DISK_TABLES BIGINT unsigned not null,"
  "CREATED_TMP_TABLES BIGINT unsigned not null,"
  "SELECT_FULL_JOIN BIGINT unsigned not null,"
  "SELECT_FULL_RANGE_JOIN BIGINT unsigned not null,"
  "SELECT_RANGE BIGINT unsigned not null,"
  "SELECT_RANGE_CHECK BIGINT unsigned not null,"
  "SELECT_SCAN BIGINT unsigned not null,"
  "SORT_MERGE_PASSES BIGINT unsigned not null,"
  "SORT_RANGE BIGINT unsigned not null,"
  "SORT_ROWS BIGINT unsigned not null,"
  "SORT_SCAN BIGINT unsigned not null,"
  "NO_INDEX_USED BIGINT unsigned not null,"
  "NO_GOOD_INDEX_USED BIGINT unsigned not null,"
  "NESTING_EVENT_ID BIGINT unsigned,"
  "NESTING_EVENT_TYPE ENUM('STATEMENT', 'STAGE', 'WAIT')"
  ")ENGINE=PERFORMANCE_SCHEMA;";

SET @str = IF(@have_pfs = 1, @cmd, 'SET @dummy = 0');
PREPARE stmt FROM @str;
EXECUTE stmt;
DROP PREPARE stmt;

--
-- TABLE EVENTS_STATEMENTS_SUMMARY_BY_THREAD_BY_EVENT_NAME
--

SET @cmd="CREATE TABLE performance_schema.events_statements_summary_by_thread_by_event_name("
  "THREAD_ID BIGINT unsigned not null,"
  "EVENT_NAME VARCHAR(128) not null,"
  "COUNT_STAR BIGINT unsigned not null,"
  "SUM_TIMER_WAIT BIGINT unsigned not null,"
  "MIN_TIMER_WAIT BIGINT unsigned not null,"
  "AVG_TIMER_WAIT BIGINT unsigned not null,"
  "MAX_TIMER_WAIT BIGINT unsigned not null,"
  "SUM_LOCK_TIME BIGINT unsigned not null,"
  "SUM_ERRORS BIGINT unsigned not null,"
  "SUM_WARNINGS BIGINT unsigned not null,"
  "SUM_ROWS_AFFECTED BIGINT unsigned not null,"
  "SUM_ROWS_SENT BIGINT unsigned not null,"
  "SUM_ROWS_EXAMINED BIGINT unsigned not null,"
  "SUM_CREATED_TMP_DISK_TABLES BIGINT unsigned not null,"
  "SUM_CREATED_TMP_TABLES BIGINT unsigned not null,"
  "SUM_SELECT_FULL_JOIN BIGINT unsigned not null,"
  "SUM_SELECT_FULL_RANGE_JOIN BIGINT unsigned not null,"
  "SUM_SELECT_RANGE BIGINT unsigned not null,"
  "SUM_SELECT_RANGE_CHECK BIGINT unsigned not null,"
  "SUM_SELECT_SCAN BIGINT unsigned not null,"
  "SUM_SORT_MERGE_PASSES BIGINT unsigned not null,"
  "SUM_SORT_RANGE BIGINT unsigned not null,"
  "SUM_SORT_ROWS BIGINT unsigned not null,"
  "SUM_SORT_SCAN BIGINT unsigned not null,"
  "SUM_NO_INDEX_USED BIGINT unsigned not null,"
  "SUM_NO_GOOD_INDEX_USED BIGINT unsigned not null"
  ")ENGINE=PERFORMANCE_SCHEMA;";

SET @str = IF(@have_pfs = 1, @cmd, 'SET @dummy = 0');
PREPARE stmt FROM @str;
EXECUTE stmt;
DROP PREPARE stmt;

--
-- TABLE EVENTS_STATEMENTS_SUMMARY_BY_HOST_BY_EVENT_NAME
--

SET @cmd="CREATE TABLE performance_schema.events_statements_summary_by_host_by_event_name("
  "HOST CHAR(60) collate utf8_bin default null,"
  "EVENT_NAME VARCHAR(128) not null,"
  "COUNT_STAR BIGINT unsigned not null,"
  "SUM_TIMER_WAIT BIGINT unsigned not null,"
  "MIN_TIMER_WAIT BIGINT unsigned not null,"
  "AVG_TIMER_WAIT BIGINT unsigned not null,"
  "MAX_TIMER_WAIT BIGINT unsigned not null,"
  "SUM_LOCK_TIME BIGINT unsigned not null,"
  "SUM_ERRORS BIGINT unsigned not null,"
  "SUM_WARNINGS BIGINT unsigned not null,"
  "SUM_ROWS_AFFECTED BIGINT unsigned not null,"
  "SUM_ROWS_SENT BIGINT unsigned not null,"
  "SUM_ROWS_EXAMINED BIGINT unsigned not null,"
  "SUM_CREATED_TMP_DISK_TABLES BIGINT unsigned not null,"
  "SUM_CREATED_TMP_TABLES BIGINT unsigned not null,"
  "SUM_SELECT_FULL_JOIN BIGINT unsigned not null,"
  "SUM_SELECT_FULL_RANGE_JOIN BIGINT unsigned not null,"
  "SUM_SELECT_RANGE BIGINT unsigned not null,"
  "SUM_SELECT_RANGE_CHECK BIGINT unsigned not null,"
  "SUM_SELECT_SCAN BIGINT unsigned not null,"
  "SUM_SORT_MERGE_PASSES BIGINT unsigned not null,"
  "SUM_SORT_RANGE BIGINT unsigned not null,"
  "SUM_SORT_ROWS BIGINT unsigned not null,"
  "SUM_SORT_SCAN BIGINT unsigned not null,"
  "SUM_NO_INDEX_USED BIGINT unsigned not null,"
  "SUM_NO_GOOD_INDEX_USED BIGINT unsigned not null"
  ")ENGINE=PERFORMANCE_SCHEMA;";

SET @str = IF(@have_pfs = 1, @cmd, 'SET @dummy = 0');
PREPARE stmt FROM @str;
EXECUTE stmt;
DROP PREPARE stmt;

--
-- TABLE EVENTS_STATEMENTS_SUMMARY_BY_USER_BY_EVENT_NAME
--

SET @cmd="CREATE TABLE performance_schema.events_statements_summary_by_user_by_event_name("
  "USER CHAR(16) collate utf8_bin default null,"
  "EVENT_NAME VARCHAR(128) not null,"
  "COUNT_STAR BIGINT unsigned not null,"
  "SUM_TIMER_WAIT BIGINT unsigned not null,"
  "MIN_TIMER_WAIT BIGINT unsigned not null,"
  "AVG_TIMER_WAIT BIGINT unsigned not null,"
  "MAX_TIMER_WAIT BIGINT unsigned not null,"
  "SUM_LOCK_TIME BIGINT unsigned not null,"
  "SUM_ERRORS BIGINT unsigned not null,"
  "SUM_WARNINGS BIGINT unsigned not null,"
  "SUM_ROWS_AFFECTED BIGINT unsigned not null,"
  "SUM_ROWS_SENT BIGINT unsigned not null,"
  "SUM_ROWS_EXAMINED BIGINT unsigned not null,"
  "SUM_CREATED_TMP_DISK_TABLES BIGINT unsigned not null,"
  "SUM_CREATED_TMP_TABLES BIGINT unsigned not null,"
  "SUM_SELECT_FULL_JOIN BIGINT unsigned not null,"
  "SUM_SELECT_FULL_RANGE_JOIN BIGINT unsigned not null,"
  "SUM_SELECT_RANGE BIGINT unsigned not null,"
  "SUM_SELECT_RANGE_CHECK BIGINT unsigned not null,"
  "SUM_SELECT_SCAN BIGINT unsigned not null,"
  "SUM_SORT_MERGE_PASSES BIGINT unsigned not null,"
  "SUM_SORT_RANGE BIGINT unsigned not null,"
  "SUM_SORT_ROWS BIGINT unsigned not null,"
  "SUM_SORT_SCAN BIGINT unsigned not null,"
  "SUM_NO_INDEX_USED BIGINT unsigned not null,"
  "SUM_NO_GOOD_INDEX_USED BIGINT unsigned not null"
  ")ENGINE=PERFORMANCE_SCHEMA;";

SET @str = IF(@have_pfs = 1, @cmd, 'SET @dummy = 0');
PREPARE stmt FROM @str;
EXECUTE stmt;
DROP PREPARE stmt;

--
-- TABLE EVENTS_STATEMENTS_SUMMARY_BY_ACCOUNT_BY_EVENT_NAME
--

SET @cmd="CREATE TABLE performance_schema.events_statements_summary_by_account_by_event_name("
  "USER CHAR(16) collate utf8_bin default null,"
  "HOST CHAR(60) collate utf8_bin default null,"
  "EVENT_NAME VARCHAR(128) not null,"
  "COUNT_STAR BIGINT unsigned not null,"
  "SUM_TIMER_WAIT BIGINT unsigned not null,"
  "MIN_TIMER_WAIT BIGINT unsigned not null,"
  "AVG_TIMER_WAIT BIGINT unsigned not null,"
  "MAX_TIMER_WAIT BIGINT unsigned not null,"
  "SUM_LOCK_TIME BIGINT unsigned not null,"
  "SUM_ERRORS BIGINT unsigned not null,"
  "SUM_WARNINGS BIGINT unsigned not null,"
  "SUM_ROWS_AFFECTED BIGINT unsigned not null,"
  "SUM_ROWS_SENT BIGINT unsigned not null,"
  "SUM_ROWS_EXAMINED BIGINT unsigned not null,"
  "SUM_CREATED_TMP_DISK_TABLES BIGINT unsigned not null,"
  "SUM_CREATED_TMP_TABLES BIGINT unsigned not null,"
  "SUM_SELECT_FULL_JOIN BIGINT unsigned not null,"
  "SUM_SELECT_FULL_RANGE_JOIN BIGINT unsigned not null,"
  "SUM_SELECT_RANGE BIGINT unsigned not null,"
  "SUM_SELECT_RANGE_CHECK BIGINT unsigned not null,"
  "SUM_SELECT_SCAN BIGINT unsigned not null,"
  "SUM_SORT_MERGE_PASSES BIGINT unsigned not null,"
  "SUM_SORT_RANGE BIGINT unsigned not null,"
  "SUM_SORT_ROWS BIGINT unsigned not null,"
  "SUM_SORT_SCAN BIGINT unsigned not null,"
  "SUM_NO_INDEX_USED BIGINT unsigned not null,"
  "SUM_NO_GOOD_INDEX_USED BIGINT unsigned not null"
  ")ENGINE=PERFORMANCE_SCHEMA;";

SET @str = IF(@have_pfs = 1, @cmd, 'SET @dummy = 0');
PREPARE stmt FROM @str;
EXECUTE stmt;
DROP PREPARE stmt;

--
-- TABLE EVENTS_STATEMENTS_SUMMARY_GLOBAL_BY_EVENT_NAME
--

SET @cmd="CREATE TABLE performance_schema.events_statements_summary_global_by_event_name("
  "EVENT_NAME VARCHAR(128) not null,"
  "COUNT_STAR BIGINT unsigned not null,"
  "SUM_TIMER_WAIT BIGINT unsigned not null,"
  "MIN_TIMER_WAIT BIGINT unsigned not null,"
  "AVG_TIMER_WAIT BIGINT unsigned not null,"
  "MAX_TIMER_WAIT BIGINT unsigned not null,"
  "SUM_LOCK_TIME BIGINT unsigned not null,"
  "SUM_ERRORS BIGINT unsigned not null,"
  "SUM_WARNINGS BIGINT unsigned not null,"
  "SUM_ROWS_AFFECTED BIGINT unsigned not null,"
  "SUM_ROWS_SENT BIGINT unsigned not null,"
  "SUM_ROWS_EXAMINED BIGINT unsigned not null,"
  "SUM_CREATED_TMP_DISK_TABLES BIGINT unsigned not null,"
  "SUM_CREATED_TMP_TABLES BIGINT unsigned not null,"
  "SUM_SELECT_FULL_JOIN BIGINT unsigned not null,"
  "SUM_SELECT_FULL_RANGE_JOIN BIGINT unsigned not null,"
  "SUM_SELECT_RANGE BIGINT unsigned not null,"
  "SUM_SELECT_RANGE_CHECK BIGINT unsigned not null,"
  "SUM_SELECT_SCAN BIGINT unsigned not null,"
  "SUM_SORT_MERGE_PASSES BIGINT unsigned not null,"
  "SUM_SORT_RANGE BIGINT unsigned not null,"
  "SUM_SORT_ROWS BIGINT unsigned not null,"
  "SUM_SORT_SCAN BIGINT unsigned not null,"
  "SUM_NO_INDEX_USED BIGINT unsigned not null,"
  "SUM_NO_GOOD_INDEX_USED BIGINT unsigned not null"
  ")ENGINE=PERFORMANCE_SCHEMA;";

SET @str = IF(@have_pfs = 1, @cmd, 'SET @dummy = 0');
PREPARE stmt FROM @str;
EXECUTE stmt;
DROP PREPARE stmt;

--
-- TABLE HOSTS
--

SET @cmd="CREATE TABLE performance_schema.hosts("
  "HOST CHAR(60) collate utf8_bin default null,"
  "CURRENT_CONNECTIONS bigint not null,"
  "TOTAL_CONNECTIONS bigint not null"
  ")ENGINE=PERFORMANCE_SCHEMA;";

SET @str = IF(@have_pfs = 1, @cmd, 'SET @dummy = 0');
PREPARE stmt FROM @str;
EXECUTE stmt;
DROP PREPARE stmt;

--
-- TABLE USERS
--

SET @cmd="CREATE TABLE performance_schema.users("
  "USER CHAR(16) collate utf8_bin default null,"
  "CURRENT_CONNECTIONS bigint not null,"
  "TOTAL_CONNECTIONS bigint not null"
  ")ENGINE=PERFORMANCE_SCHEMA;";

SET @str = IF(@have_pfs = 1, @cmd, 'SET @dummy = 0');
PREPARE stmt FROM @str;
EXECUTE stmt;
DROP PREPARE stmt;

--
-- TABLE ACCOUNTS
--

SET @cmd="CREATE TABLE performance_schema.accounts("
  "USER CHAR(16) collate utf8_bin default null,"
  "HOST CHAR(60) collate utf8_bin default null,"
  "CURRENT_CONNECTIONS bigint not null,"
  "TOTAL_CONNECTIONS bigint not null"
  ")ENGINE=PERFORMANCE_SCHEMA;";

SET @str = IF(@have_pfs = 1, @cmd, 'SET @dummy = 0');
PREPARE stmt FROM @str;
EXECUTE stmt;
DROP PREPARE stmt;

--
-- TABLE EVENTS_STATEMENTS_SUMMARY_BY_DIGEST
--

SET @cmd="CREATE TABLE performance_schema.events_statements_summary_by_digest("
  "SCHEMA_NAME VARCHAR(64),"
  "DIGEST VARCHAR(32),"
  "DIGEST_TEXT LONGTEXT,"
  "COUNT_STAR BIGINT unsigned not null,"
  "SUM_TIMER_WAIT BIGINT unsigned not null,"
  "MIN_TIMER_WAIT BIGINT unsigned not null,"
  "AVG_TIMER_WAIT BIGINT unsigned not null,"
  "MAX_TIMER_WAIT BIGINT unsigned not null,"
  "SUM_LOCK_TIME BIGINT unsigned not null,"
  "SUM_ERRORS BIGINT unsigned not null,"
  "SUM_WARNINGS BIGINT unsigned not null,"
  "SUM_ROWS_AFFECTED BIGINT unsigned not null,"
  "SUM_ROWS_SENT BIGINT unsigned not null,"
  "SUM_ROWS_EXAMINED BIGINT unsigned not null,"
  "SUM_CREATED_TMP_DISK_TABLES BIGINT unsigned not null,"
  "SUM_CREATED_TMP_TABLES BIGINT unsigned not null,"
  "SUM_SELECT_FULL_JOIN BIGINT unsigned not null,"
  "SUM_SELECT_FULL_RANGE_JOIN BIGINT unsigned not null,"
  "SUM_SELECT_RANGE BIGINT unsigned not null,"
  "SUM_SELECT_RANGE_CHECK BIGINT unsigned not null,"
  "SUM_SELECT_SCAN BIGINT unsigned not null,"
  "SUM_SORT_MERGE_PASSES BIGINT unsigned not null,"
  "SUM_SORT_RANGE BIGINT unsigned not null,"
  "SUM_SORT_ROWS BIGINT unsigned not null,"
  "SUM_SORT_SCAN BIGINT unsigned not null,"
  "SUM_NO_INDEX_USED BIGINT unsigned not null,"
  "SUM_NO_GOOD_INDEX_USED BIGINT unsigned not null,"
  "FIRST_SEEN TIMESTAMP(0) NOT NULL default 0,"
  "LAST_SEEN TIMESTAMP(0) NOT NULL default 0"
  ")ENGINE=PERFORMANCE_SCHEMA;";


SET @str = IF(@have_pfs = 1, @cmd, 'SET @dummy = 0');
PREPARE stmt FROM @str;
EXECUTE stmt;
DROP PREPARE stmt;

--
-- TABLE SESSION_CONNECT_ATTRS
--

SET @cmd="CREATE TABLE performance_schema.session_connect_attrs("
  "PROCESSLIST_ID INT NOT NULL,"
  "ATTR_NAME VARCHAR(32) NOT NULL,"
  "ATTR_VALUE VARCHAR(1024),"
  "ORDINAL_POSITION INT"
  ")ENGINE=PERFORMANCE_SCHEMA CHARACTER SET utf8 COLLATE utf8_bin;";

SET @str = IF(@have_pfs = 1, @cmd, 'SET @dummy = 0');
PREPARE stmt FROM @str;
EXECUTE stmt;
DROP PREPARE stmt;

--
-- TABLE SESSION_ACCOUNT_CONNECT_ATTRS
--

SET @cmd="CREATE TABLE performance_schema.session_account_connect_attrs "
         " LIKE performance_schema.session_connect_attrs;";

SET @str = IF(@have_pfs = 1, @cmd, 'SET @dummy = 0');
PREPARE stmt FROM @str;
EXECUTE stmt;
DROP PREPARE stmt;
