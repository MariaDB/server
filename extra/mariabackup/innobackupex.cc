/******************************************************
hot backup tool for InnoDB
(c) 2009-2015 Percona LLC and/or its affiliates
Originally Created 3/3/2009 Yasufumi Kinoshita
Written by Alexey Kopytov, Aleksandr Kuzminsky, Stewart Smith, Vadim Tkachenko,
Yasufumi Kinoshita, Ignacio Nin and Baron Schwartz.

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; version 2 of the License.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1335  USA

*******************************************************

This file incorporates work covered by the following copyright and
permission notice:

Copyright (c) 2000, 2011, MySQL AB & Innobase Oy. All Rights Reserved.

This program is free software; you can redistribute it and/or modify it under
the terms of the GNU General Public License as published by the Free Software
Foundation; version 2 of the License.

This program is distributed in the hope that it will be useful, but WITHOUT
ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License along with
this program; if not, write to the Free Software Foundation, Inc., 51 Franklin
Street, Fifth Floor, Boston, MA 02110-1335 USA

*******************************************************/

#include <my_global.h>
#include <stdio.h>
#include <string.h>
#include <mysql.h>
#include <my_dir.h>
#include <ut0mem.h>
#include <os0file.h>
#include <srv0start.h>
#include <algorithm>
#include <mysqld.h>
#include <my_default.h>
#include <my_getopt.h>
#include <string>
#include <sstream>
#include <set>
#include "common.h"
#include "innobackupex.h"
#include "xtrabackup.h"
#include "xbstream.h"
#include "fil_cur.h"
#include "write_filt.h"
#include "backup_copy.h"

using std::min;
using std::max;

/* options */
my_bool opt_ibx_version = FALSE;
my_bool opt_ibx_help = FALSE;
my_bool opt_ibx_apply_log = FALSE;
my_bool opt_ibx_incremental = FALSE;
my_bool opt_ibx_notimestamp = FALSE;

my_bool opt_ibx_copy_back = FALSE;
my_bool opt_ibx_move_back = FALSE;
my_bool opt_ibx_galera_info = FALSE;
my_bool opt_ibx_slave_info = FALSE;
my_bool opt_ibx_no_lock = FALSE;
my_bool opt_ibx_safe_slave_backup = FALSE;
my_bool opt_ibx_rsync = FALSE;
my_bool opt_ibx_force_non_empty_dirs = FALSE;
my_bool opt_ibx_noversioncheck = FALSE;
my_bool opt_ibx_no_backup_locks = FALSE;
my_bool opt_ibx_decompress = FALSE;

char *opt_ibx_incremental_history_name = NULL;
char *opt_ibx_incremental_history_uuid = NULL;

char *opt_ibx_user = NULL;
char *opt_ibx_password = NULL;
char *opt_ibx_host = NULL;
char *opt_ibx_defaults_group = NULL;
char *opt_ibx_socket = NULL;
uint opt_ibx_port = 0;

ulong opt_ibx_lock_wait_query_type;
ulong opt_ibx_kill_long_query_type;

uint opt_ibx_kill_long_queries_timeout = 0;
uint opt_ibx_lock_wait_timeout = 0;
uint opt_ibx_lock_wait_threshold = 0;
uint opt_ibx_debug_sleep_before_unlock = 0;
uint opt_ibx_safe_slave_backup_timeout = 0;

const char *opt_ibx_history = NULL;

char *opt_ibx_include = NULL;
char *opt_ibx_databases = NULL;
bool ibx_partial_backup = false;

char *ibx_position_arg = NULL;
char *ibx_backup_directory = NULL;

/* copy of proxied xtrabackup options */
my_bool ibx_xb_close_files;
const char *ibx_xtrabackup_compress_alg;
uint ibx_xtrabackup_compress_threads;
ulonglong ibx_xtrabackup_compress_chunk_size;
my_bool ibx_xtrabackup_export;
char *ibx_xtrabackup_extra_lsndir;
char *ibx_xtrabackup_incremental_basedir;
char *ibx_xtrabackup_incremental_dir;
my_bool	ibx_xtrabackup_incremental_force_scan;
ulint ibx_xtrabackup_log_copy_interval;
char *ibx_xtrabackup_incremental;
int ibx_xtrabackup_parallel;
char *ibx_xtrabackup_stream_str;
char *ibx_xtrabackup_tables_file;
long ibx_xtrabackup_throttle;
char *ibx_opt_mysql_tmpdir;
longlong ibx_xtrabackup_use_memory;


static inline int ibx_msg(const char *fmt, ...) ATTRIBUTE_FORMAT(printf, 1, 2);
static inline int ibx_msg(const char *fmt, ...)
{
	int	result;
	time_t	t = time(NULL);
	char	date[100];
	char	*line;
	va_list args;

	strftime(date, sizeof(date), "%y%m%d %H:%M:%S", localtime(&t));

	va_start(args, fmt);

	result = vasprintf(&line, fmt, args);

	va_end(args);

	if (result != -1) {
		result = fprintf(stderr, "%s %s: %s",
					date, INNOBACKUPEX_BIN_NAME, line);
		free(line);
	}

	return result;
}

enum innobackupex_options
{
	OPT_APPLY_LOG = 256,
	OPT_COPY_BACK,
	OPT_MOVE_BACK,
	OPT_REDO_ONLY,
	OPT_GALERA_INFO,
	OPT_SLAVE_INFO,
	OPT_INCREMENTAL,
	OPT_INCREMENTAL_HISTORY_NAME,
	OPT_INCREMENTAL_HISTORY_UUID,
	OPT_LOCK_WAIT_QUERY_TYPE,
	OPT_KILL_LONG_QUERY_TYPE,
	OPT_KILL_LONG_QUERIES_TIMEOUT,
	OPT_LOCK_WAIT_TIMEOUT,
	OPT_LOCK_WAIT_THRESHOLD,
	OPT_DEBUG_SLEEP_BEFORE_UNLOCK,
	OPT_NO_LOCK,
	OPT_SAFE_SLAVE_BACKUP,
	OPT_SAFE_SLAVE_BACKUP_TIMEOUT,
	OPT_RSYNC,
	OPT_HISTORY,
	OPT_INCLUDE,
	OPT_FORCE_NON_EMPTY_DIRS,
	OPT_NO_TIMESTAMP,
	OPT_NO_VERSION_CHECK,
	OPT_NO_BACKUP_LOCKS,
	OPT_DATABASES,
	OPT_DECOMPRESS,

	/* options wich are passed directly to xtrabackup */
	OPT_CLOSE_FILES,
	OPT_COMPACT,
	OPT_COMPRESS,
	OPT_COMPRESS_THREADS,
	OPT_COMPRESS_CHUNK_SIZE,
	OPT_EXPORT,
	OPT_EXTRA_LSNDIR,
	OPT_INCREMENTAL_BASEDIR,
	OPT_INCREMENTAL_DIR,
	OPT_INCREMENTAL_FORCE_SCAN,
	OPT_LOG_COPY_INTERVAL,
	OPT_PARALLEL,
	OPT_REBUILD_INDEXES,
	OPT_REBUILD_THREADS,
	OPT_STREAM,
	OPT_TABLES_FILE,
	OPT_THROTTLE,
	OPT_USE_MEMORY,
	OPT_INNODB_FORCE_RECOVERY,
};

ibx_mode_t ibx_mode = IBX_MODE_BACKUP;

static struct my_option ibx_long_options[] =
{
	{"version", 'v', "print version information",
	 (uchar *) &opt_ibx_version, (uchar *) &opt_ibx_version, 0,
	 GET_BOOL, NO_ARG, 0, 0, 0, 0, 0, 0},

	{"help", '?', "This option displays a help screen and exits.",
	 (uchar *) &opt_ibx_help, (uchar *) &opt_ibx_help, 0,
	 GET_BOOL, NO_ARG, 0, 0, 0, 0, 0, 0},

	{"apply-log", OPT_APPLY_LOG, "Prepare a backup in BACKUP-DIR by "
	"applying the redo log 'ib_logfile0' and creating new redo log. "
	"The InnoDB configuration is read from the file \"backup-my.cnf\".",
	(uchar*) &opt_ibx_apply_log, (uchar*) &opt_ibx_apply_log,
	0, GET_BOOL, NO_ARG, 0, 0, 0, 0, 0, 0},

	{"copy-back", OPT_COPY_BACK, "Copy all the files in a previously made "
	 "backup from the backup directory to their original locations.",
	 (uchar *) &opt_ibx_copy_back, (uchar *) &opt_ibx_copy_back, 0,
	 GET_BOOL, NO_ARG, 0, 0, 0, 0, 0, 0},

	{"move-back", OPT_MOVE_BACK, "Move all the files in a previously made "
	 "backup from the backup directory to the actual datadir location. "
	 "Use with caution, as it removes backup files.",
	 (uchar *) &opt_ibx_move_back, (uchar *) &opt_ibx_move_back, 0,
	 GET_BOOL, NO_ARG, 0, 0, 0, 0, 0, 0},

	{"galera-info", OPT_GALERA_INFO, "This options creates the "
	 "xtrabackup_galera_info file which contains the local node state at "
	 "the time of the backup. Option should be used when performing the "
	 "backup of MariaDB Galera Cluster. Has no effect when backup locks "
	 "are used to create the backup.",
	 (uchar *) &opt_ibx_galera_info, (uchar *) &opt_ibx_galera_info, 0,
	 GET_BOOL, NO_ARG, 0, 0, 0, 0, 0, 0},

	{"slave-info", OPT_SLAVE_INFO, "This option is useful when backing "
	 "up a replication slave server. It prints the binary log position "
	 "and name of the master server. It also writes this information to "
	 "the \"xtrabackup_slave_info\" file as a \"CHANGE MASTER\" command. "
	 "A new slave for this master can be set up by starting a slave server "
	 "on this backup and issuing a \"CHANGE MASTER\" command with the "
	 "binary log position saved in the \"xtrabackup_slave_info\" file.",
	 (uchar *) &opt_ibx_slave_info, (uchar *) &opt_ibx_slave_info, 0,
	 GET_BOOL, NO_ARG, 0, 0, 0, 0, 0, 0},

	{"incremental", OPT_INCREMENTAL,
	 "Create an incremental backup, rather than a full one. When this option is specified, "
	 "either --incremental-lsn or --incremental-basedir can also be given. "
	 "If neither option is given, option --incremental-basedir is used "
	 "by default, set to the first timestamped backup "
	 "directory in the backup base directory.",
	 (uchar *) &opt_ibx_incremental, (uchar *) &opt_ibx_incremental, 0,
	 GET_BOOL, NO_ARG, 0, 0, 0, 0, 0, 0},

	{"no-lock", OPT_NO_LOCK, "Use this option to disable table lock "
	 "with \"FLUSH TABLES WITH READ LOCK\". Use it only if ALL your "
	 "tables are InnoDB and you DO NOT CARE about the binary log "
	 "position of the backup. This option shouldn't be used if there "
	 "are any DDL statements being executed or if any updates are "
	 "happening on non-InnoDB tables (this includes the system MyISAM "
	 "tables in the mysql database), otherwise it could lead to an "
	 "inconsistent backup. If you are considering to use --no-lock "
	 "because your backups are failing to acquire the lock, this could "
	 "be because of incoming replication events preventing the lock "
	 "from succeeding. Please try using --safe-slave-backup to "
	 "momentarily stop the replication slave thread, this may help "
	 "the backup to succeed and you then don't need to resort to "
	 "using this option.",
	 (uchar *) &opt_ibx_no_lock, (uchar *) &opt_ibx_no_lock, 0,
	 GET_BOOL, NO_ARG, 0, 0, 0, 0, 0, 0},

	{"safe-slave-backup", OPT_SAFE_SLAVE_BACKUP, "Stop slave SQL thread "
	 "and wait to start backup until Slave_open_temp_tables in "
	 "\"SHOW STATUS\" is zero. If there are no open temporary tables, "
	 "the backup will take place, otherwise the SQL thread will be "
	 "started and stopped until there are no open temporary tables. "
	 "The backup will fail if Slave_open_temp_tables does not become "
	 "zero after --safe-slave-backup-timeout seconds. The slave SQL "
	 "thread will be restarted when the backup finishes.",
	 (uchar *) &opt_ibx_safe_slave_backup,
	 (uchar *) &opt_ibx_safe_slave_backup,
	 0, GET_BOOL, NO_ARG, 0, 0, 0, 0, 0, 0},

	{"rsync", OPT_RSYNC, "Uses the rsync utility to optimize local file "
	 "transfers. When this option is specified, innobackupex uses rsync "
	 "to copy all non-InnoDB files instead of spawning a separate cp for "
	 "each file, which can be much faster for servers with a large number "
	 "of databases or tables.  This option cannot be used together with "
	 "--stream.",
	 (uchar *) &opt_ibx_rsync, (uchar *) &opt_ibx_rsync,
	 0, GET_BOOL, NO_ARG, 0, 0, 0, 0, 0, 0},

	{"force-non-empty-directories", OPT_FORCE_NON_EMPTY_DIRS, "This "
	 "option, when specified, makes --copy-back or --move-back transfer "
	 "files to non-empty directories. Note that no existing files will be "
	 "overwritten. If --copy-back or --move-back has to copy a file from "
	 "the backup directory which already exists in the destination "
	 "directory, it will still fail with an error.",
	 (uchar *) &opt_ibx_force_non_empty_dirs,
	 (uchar *) &opt_ibx_force_non_empty_dirs,
	 0, GET_BOOL, NO_ARG, 0, 0, 0, 0, 0, 0},

	{"no-timestamp", OPT_NO_TIMESTAMP, "This option prevents creation of a "
	 "time-stamped subdirectory of the BACKUP-ROOT-DIR given on the "
	 "command line. When it is specified, the backup is done in "
	 "BACKUP-ROOT-DIR instead.",
	 (uchar *) &opt_ibx_notimestamp,
	 (uchar *) &opt_ibx_notimestamp,
	 0, GET_BOOL, NO_ARG, 0, 0, 0, 0, 0, 0},

	{"no-version-check", OPT_NO_VERSION_CHECK, "This option disables the "
	 "version check which is enabled by the --version-check option.",
	 (uchar *) &opt_ibx_noversioncheck,
	 (uchar *) &opt_ibx_noversioncheck,
	 0, GET_BOOL, NO_ARG, 0, 0, 0, 0, 0, 0},

	{"no-backup-locks", OPT_NO_BACKUP_LOCKS, "This option controls if "
	 "backup locks should be used instead of FLUSH TABLES WITH READ LOCK "
	 "on the backup stage. The option has no effect when backup locks are "
	 "not supported by the server. This option is enabled by default, "
	 "disable with --no-backup-locks.",
	 (uchar *) &opt_ibx_no_backup_locks,
	 (uchar *) &opt_ibx_no_backup_locks,
	 0, GET_BOOL, NO_ARG, 0, 0, 0, 0, 0, 0},

	{"decompress", OPT_DECOMPRESS, "Decompresses all files with the .qp "
	 "extension in a backup previously made with the --compress option.",
	 (uchar *) &opt_ibx_decompress,
	 (uchar *) &opt_ibx_decompress,
	 0, GET_BOOL, NO_ARG, 0, 0, 0, 0, 0, 0},

	{"user", 'u', "This option specifies the MySQL username used "
	 "when connecting to the server, if that's not the current user. "
	 "The option accepts a string argument. See mysql --help for details.",
	 (uchar*) &opt_ibx_user, (uchar*) &opt_ibx_user, 0, GET_STR,
	 REQUIRED_ARG, 0, 0, 0, 0, 0, 0},

	{"host", 'H', "This option specifies the host to use when "
	 "connecting to the database server with TCP/IP.  The option accepts "
	 "a string argument. See mysql --help for details.",
	 (uchar*) &opt_ibx_host, (uchar*) &opt_ibx_host, 0, GET_STR,
	 REQUIRED_ARG, 0, 0, 0, 0, 0, 0},

	{"port", 'P', "This option specifies the port to use when "
	 "connecting to the database server with TCP/IP.  The option accepts "
	 "a string argument. See mysql --help for details.",
	 &opt_ibx_port, &opt_ibx_port, 0, GET_UINT, REQUIRED_ARG,
	 0, 0, 0, 0, 0, 0},

	{"password", 'p', "This option specifies the password to use "
	 "when connecting to the database. It accepts a string argument.  "
	 "See mysql --help for details.",
	 0, 0, 0, GET_STR,
	 REQUIRED_ARG, 0, 0, 0, 0, 0, 0},

	{"socket", 'S', "This option specifies the socket to use when "
	 "connecting to the local database server with a UNIX domain socket.  "
	 "The option accepts a string argument. See mysql --help for details.",
	 (uchar*) &opt_ibx_socket, (uchar*) &opt_ibx_socket, 0, GET_STR,
	 REQUIRED_ARG, 0, 0, 0, 0, 0, 0},

	{"incremental-history-name", OPT_INCREMENTAL_HISTORY_NAME,
	 "This option specifies the name of the backup series stored in the "
	 "PERCONA_SCHEMA.xtrabackup_history history record to base an "
	 "incremental backup on. Backup will search the history table "
	 "looking for the most recent (highest innodb_to_lsn), successful "
	 "backup in the series and take the to_lsn value to use as the "
	 "starting lsn for the incremental backup. This will be mutually "
	 "exclusive with --incremental-history-uuid, --incremental-basedir "
	 "and --incremental-lsn. If no valid lsn can be found (no series by "
	 "that name, no successful backups by that name), "
	 "an error will be returned. It is used with the --incremental option.",
	 (uchar*) &opt_ibx_incremental_history_name,
	 (uchar*) &opt_ibx_incremental_history_name, 0, GET_STR,
	 REQUIRED_ARG, 0, 0, 0, 0, 0, 0},

	{"incremental-history-uuid", OPT_INCREMENTAL_HISTORY_UUID,
	 "This option specifies the UUID of the specific history record "
	 "stored in the PERCONA_SCHEMA.xtrabackup_history to base an "
	 "incremental backup on. --incremental-history-name, "
	 "--incremental-basedir and --incremental-lsn. If no valid lsn can be "
	 "found (no success record with that uuid), an error will be returned."
	 " It is used with the --incremental option.",
	 (uchar*) &opt_ibx_incremental_history_uuid,
	 (uchar*) &opt_ibx_incremental_history_uuid, 0, GET_STR,
	 REQUIRED_ARG, 0, 0, 0, 0, 0, 0},

	{"ftwrl-wait-query-type", OPT_LOCK_WAIT_QUERY_TYPE,
	 "This option specifies which types of queries are allowed to complete "
	 "before innobackupex will issue the global lock. Default is all.",
	 (uchar*) &opt_ibx_lock_wait_query_type,
	 (uchar*) &opt_ibx_lock_wait_query_type, &query_type_typelib,
	 GET_ENUM, REQUIRED_ARG, QUERY_TYPE_ALL, 0, 0, 0, 0, 0},

	{"kill-long-query-type", OPT_KILL_LONG_QUERY_TYPE,
	 "This option specifies which types of queries should be killed to "
	 "unblock the global lock. Default is \"all\".",
	 (uchar*) &opt_ibx_kill_long_query_type,
	 (uchar*) &opt_ibx_kill_long_query_type, &query_type_typelib,
	 GET_ENUM, REQUIRED_ARG, QUERY_TYPE_SELECT, 0, 0, 0, 0, 0},

	{"history", OPT_HISTORY,
	 "This option enables the tracking of backup history in the "
	 "PERCONA_SCHEMA.xtrabackup_history table. An optional history "
	 "series name may be specified that will be placed with the history "
	 "record for the current backup being taken.",
	 NULL, NULL, 0, GET_STR, OPT_ARG, 0, 0, 0, 0, 0, 0},

	{"include", OPT_INCLUDE,
	 "This option is a regular expression to be matched against table "
	 "names in databasename.tablename format. It is passed directly to "
	 "--tables option. See the documentation for "
	 "details.",
	 (uchar*) &opt_ibx_include,
	 (uchar*) &opt_ibx_include, 0, GET_STR,
	 REQUIRED_ARG, 0, 0, 0, 0, 0, 0},

	{"databases", OPT_DATABASES,
	 "This option specifies the list of databases that innobackupex should "
	 "back up. The option accepts a string argument or path to file that "
	 "contains the list of databases to back up. The list is of the form "
	 "\"databasename1[.table_name1] databasename2[.table_name2] . . .\". "
	 "If this option is not specified, all databases containing MyISAM and "
	 "InnoDB tables will be backed up.  Please make sure that --databases "
	 "contains all of the InnoDB databases and tables, so that all of the "
	 "innodb.frm files are also backed up. In case the list is very long, "
	 "this can be specified in a file, and the full path of the file can "
	 "be specified instead of the list. (See option --tables-file.)",
	 (uchar*) &opt_ibx_databases,
	 (uchar*) &opt_ibx_databases, 0, GET_STR,
	 REQUIRED_ARG, 0, 0, 0, 0, 0, 0},

	{"kill-long-queries-timeout", OPT_KILL_LONG_QUERIES_TIMEOUT,
	 "This option specifies the number of seconds innobackupex waits "
	 "between starting FLUSH TABLES WITH READ LOCK and killing those "
	 "queries that block it. Default is 0 seconds, which means "
	 "innobackupex will not attempt to kill any queries.",
	 (uchar*) &opt_ibx_kill_long_queries_timeout,
	 (uchar*) &opt_ibx_kill_long_queries_timeout, 0, GET_UINT,
	 REQUIRED_ARG, 0, 0, 0, 0, 0, 0},

	{"ftwrl-wait-timeout", OPT_LOCK_WAIT_TIMEOUT,
	 "This option specifies time in seconds that innobackupex should wait "
	 "for queries that would block FTWRL before running it. If there are "
	 "still such queries when the timeout expires, innobackupex terminates "
	 "with an error. Default is 0, in which case innobackupex does not "
	 "wait for queries to complete and starts FTWRL immediately.",
	 (uchar*) &opt_ibx_lock_wait_timeout,
	 (uchar*) &opt_ibx_lock_wait_timeout, 0, GET_UINT,
	 REQUIRED_ARG, 0, 0, 0, 0, 0, 0},

	{"ftwrl-wait-threshold", OPT_LOCK_WAIT_THRESHOLD,
	 "This option specifies the query run time threshold which is used by "
	 "innobackupex to detect long-running queries with a non-zero value "
	 "of --ftwrl-wait-timeout. FTWRL is not started until such "
	 "long-running queries exist. This option has no effect if "
	 "--ftwrl-wait-timeout is 0. Default value is 60 seconds.",
	 (uchar*) &opt_ibx_lock_wait_threshold,
	 (uchar*) &opt_ibx_lock_wait_threshold, 0, GET_UINT,
	 REQUIRED_ARG, 60, 0, 0, 0, 0, 0},

	{"safe-slave-backup-timeout", OPT_SAFE_SLAVE_BACKUP_TIMEOUT,
	 "How many seconds --safe-slave-backup should wait for "
	 "Slave_open_temp_tables to become zero. (default 300)",
	 (uchar*) &opt_ibx_safe_slave_backup_timeout,
	 (uchar*) &opt_ibx_safe_slave_backup_timeout, 0, GET_UINT,
	 REQUIRED_ARG, 300, 0, 0, 0, 0, 0},


	/* Following command-line options are actually handled by xtrabackup.
	We put them here with only purpose for them to showup in
	innobackupex --help output */

	{"close_files", OPT_CLOSE_FILES, "Do not keep files opened."
	" Use at your own risk.",
	 (uchar*) &ibx_xb_close_files, (uchar*) &ibx_xb_close_files, 0,
	 GET_BOOL, NO_ARG, 0, 0, 0, 0, 0, 0},

	{"compress", OPT_COMPRESS, "This option instructs backup to "
	 "compress backup copies of InnoDB data files."
	 , (uchar*) &ibx_xtrabackup_compress_alg,
	 (uchar*) &ibx_xtrabackup_compress_alg, 0,
	 GET_STR, OPT_ARG, 0, 0, 0, 0, 0, 0},

	{"compress-threads", OPT_COMPRESS_THREADS,
	 "This option specifies the number of worker threads that will be used "
	 "for parallel compression.",
	 (uchar*) &ibx_xtrabackup_compress_threads,
	 (uchar*) &ibx_xtrabackup_compress_threads,
	 0, GET_UINT, REQUIRED_ARG, 1, 1, UINT_MAX, 0, 0, 0},

	{"compress-chunk-size", OPT_COMPRESS_CHUNK_SIZE, "Size of working "
	 "buffer(s) for compression threads in bytes. The default value "
	 "is 64K.", (uchar*) &ibx_xtrabackup_compress_chunk_size,
	 (uchar*) &ibx_xtrabackup_compress_chunk_size,
	 0, GET_ULL, REQUIRED_ARG, (1 << 16), 1024, ULONGLONG_MAX, 0, 0, 0},

	{"export", OPT_EXPORT, " enables exporting individual tables for import "
	 "into another server.",
	 (uchar*) &ibx_xtrabackup_export, (uchar*) &ibx_xtrabackup_export,
	 0, GET_BOOL, NO_ARG, 0, 0, 0, 0, 0, 0},

	{"extra-lsndir", OPT_EXTRA_LSNDIR, "This option specifies the "
	 "directory in which to save an extra copy of the "
	 "\"xtrabackup_checkpoints\" file. The option accepts a string "
	 "argument.",
	 (uchar*) &ibx_xtrabackup_extra_lsndir,
	 (uchar*) &ibx_xtrabackup_extra_lsndir,
	 0, GET_STR, REQUIRED_ARG, 0, 0, 0, 0, 0, 0},

	{"incremental-basedir", OPT_INCREMENTAL_BASEDIR, "This option "
	 "specifies the directory containing the full backup that is the base "
	 "dataset for the incremental backup.  The option accepts a string "
	 "argument. It is used with the --incremental option.",
	 (uchar*) &ibx_xtrabackup_incremental_basedir,
	 (uchar*) &ibx_xtrabackup_incremental_basedir,
	 0, GET_STR, REQUIRED_ARG, 0, 0, 0, 0, 0, 0},

	{"incremental-dir", OPT_INCREMENTAL_DIR, "This option specifies the "
	 "directory where the incremental backup will be combined with the "
	 "full backup to make a new full backup.  The option accepts a string "
	 "argument. It is used with the --incremental option.",
	 (uchar*) &ibx_xtrabackup_incremental_dir,
	 (uchar*) &ibx_xtrabackup_incremental_dir,
	 0, GET_STR, REQUIRED_ARG, 0, 0, 0, 0, 0, 0},

	{"incremental-force-scan", OPT_INCREMENTAL_FORCE_SCAN,
	 "Perform full scan of data files "
	 "for taking an incremental backup even if full changed page bitmap "
	 "data is available to enable the backup without the full scan.",
	 (uchar*)&ibx_xtrabackup_incremental_force_scan,
	 (uchar*)&ibx_xtrabackup_incremental_force_scan, 0, GET_BOOL, NO_ARG,
	 0, 0, 0, 0, 0, 0},

	{"log-copy-interval", OPT_LOG_COPY_INTERVAL, "This option specifies "
	 "time interval between checks done by log copying thread in "
	 "milliseconds.", (uchar*) &ibx_xtrabackup_log_copy_interval,
	 (uchar*) &ibx_xtrabackup_log_copy_interval,
	 0, GET_LONG, REQUIRED_ARG, 1000, 0, LONG_MAX, 0, 1, 0},

	{"incremental-lsn", OPT_INCREMENTAL, "This option specifies the log "
	 "sequence number (LSN) to use for the incremental backup.  The option "
	 "accepts a string argument. It is used with the --incremental option. "
	 "It is used instead of specifying --incremental-basedir. For "
	 "databases created by MySQL and Percona Server 5.0-series versions, "
	 "specify the LSN as two 32-bit integers in high:low format. For "
	 "databases created in 5.1 and later, specify the LSN as a single "
	 "64-bit integer.",
	 (uchar*) &ibx_xtrabackup_incremental,
	 (uchar*) &ibx_xtrabackup_incremental,
	 0, GET_STR, REQUIRED_ARG, 0, 0, 0, 0, 0, 0},

	{"parallel", OPT_PARALLEL, "On backup, this option specifies the "
	 "number of threads to use to back "
	 "up files concurrently.  The option accepts an integer argument.",
	 (uchar*) &ibx_xtrabackup_parallel, (uchar*) &ibx_xtrabackup_parallel,
	 0, GET_INT, REQUIRED_ARG, 1, 1, INT_MAX, 0, 0, 0},


	{"stream", OPT_STREAM, "This option specifies the format in which to "
	 "do the streamed backup.  The option accepts a string argument. The "
	 "backup will be done to STDOUT in the specified format. Currently, "
	 "the only supported formats are tar and mbstream/xbstream.",
	 (uchar*) &ibx_xtrabackup_stream_str,
	 (uchar*) &ibx_xtrabackup_stream_str, 0, GET_STR,
	 REQUIRED_ARG, 0, 0, 0, 0, 0, 0},

	{"tables-file", OPT_TABLES_FILE, "This option specifies the file in "
	 "which there are a list of names of the form database.  The option "
	 "accepts a string argument.table, one per line.",
	 (uchar*) &ibx_xtrabackup_tables_file,
	 (uchar*) &ibx_xtrabackup_tables_file,
	 0, GET_STR, REQUIRED_ARG, 0, 0, 0, 0, 0, 0},

	{"throttle", OPT_THROTTLE, "This option specifies a number of I/O "
	 "operations (pairs of read+write) per second.  It accepts an integer "
	 "argument.",
	 (uchar*) &ibx_xtrabackup_throttle, (uchar*) &ibx_xtrabackup_throttle,
	 0, GET_LONG, REQUIRED_ARG, 0, 0, LONG_MAX, 0, 1, 0},

	{"tmpdir", 't', "This option specifies the location where a temporary "
	 "files will be stored. If the option is not specified, the default is "
	 "to use the value of tmpdir read from the server configuration.",
	 (uchar*) &ibx_opt_mysql_tmpdir,
	 (uchar*) &ibx_opt_mysql_tmpdir, 0, GET_STR, REQUIRED_ARG,
	 0, 0, 0, 0, 0, 0},

	{"use-memory", OPT_USE_MEMORY, "This option accepts a string argument "
	 "that specifies the amount of memory in bytes to use "
	 "for crash recovery while preparing a backup. Multiples are supported "
	 "providing the unit (e.g. 1MB, 1GB). It is used only with the option "
	 "--apply-log.",
	 (uchar*) &ibx_xtrabackup_use_memory,
	 (uchar*) &ibx_xtrabackup_use_memory,
	 0, GET_LL, REQUIRED_ARG, 100*1024*1024L, 1024*1024L, LONGLONG_MAX, 0,
	 1024*1024L, 0},

	{"innodb-force-recovery", OPT_INNODB_FORCE_RECOVERY,
	 "This option starts up the embedded InnoDB instance in crash "
	 "recovery mode to ignore page corruption; should be used "
	 "with the \"--apply-log\" option, in emergencies only. The "
	 "default value is 0. Refer to \"innodb_force_recovery\" server "
	 "system variable documentation for more details.",
	 (uchar*)&xtrabackup_innodb_force_recovery,
	 (uchar*)&xtrabackup_innodb_force_recovery,
	 0, GET_ULONG, OPT_ARG, 0, 0, SRV_FORCE_IGNORE_CORRUPT, 0, 0, 0},

	{ 0, 0, 0, 0, 0, 0, GET_NO_ARG, NO_ARG, 0, 0, 0, 0, 0, 0}
};


static void usage(void)
{
	puts("Open source backup tool\n\
\n\
Copyright (C) 2009-2015 Percona LLC and/or its affiliates.\n\
Portions Copyright (C) 2000, 2011, MySQL AB & Innobase Oy. All Rights Reserved.\n\
\n\
This program is free software; you can redistribute it and/or\n\
modify it under the terms of the GNU General Public License\n\
as published by the Free Software Foundation version 2\n\
of the License.\n\
\n\
This program is distributed in the hope that it will be useful,\n\
but WITHOUT ANY WARRANTY; without even the implied warranty of\n\
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the\n\
GNU General Public License for more details.\n\
\n\
You can download full text of the license on http://www.gnu.org/licenses/gpl-2.0.txt\n\n");

	puts("innobackupex - Non-blocking backup tool for InnoDB, XtraDB and HailDB databases\n\
\n\
SYNOPOSIS\n\
\n\
innobackupex [--compress] [--compress-threads=NUMBER-OF-THREADS] [--compress-chunk-size=CHUNK-SIZE]\n\
             [--include=REGEXP] [--user=NAME]\n\
             [--password=WORD] [--port=PORT] [--socket=SOCKET]\n\
             [--no-timestamp] [--ibbackup=IBBACKUP-BINARY]\n\
             [--slave-info] [--galera-info] [--stream=tar|mbstream|xbstream]\n\
             [--defaults-file=MY.CNF] [--defaults-group=GROUP-NAME]\n\
             [--databases=LIST] [--no-lock] \n\
             [--tmpdir=DIRECTORY] [--tables-file=FILE]\n\
             [--history=NAME]\n\
             [--incremental] [--incremental-basedir]\n\
             [--incremental-dir] [--incremental-force-scan] [--incremental-lsn]\n\
             [--incremental-history-name=NAME] [--incremental-history-uuid=UUID]\n\
             [--close-files]\n\
             BACKUP-ROOT-DIR\n\
\n\
innobackupex --apply-log [--use-memory=B]\n\
             [--defaults-file=MY.CNF]\n\
             [--export] [--ibbackup=IBBACKUP-BINARY]\n\
             [--innodb-force-recovery=1]\n\
             BACKUP-DIR\n\
\n\
innobackupex --copy-back [--defaults-file=MY.CNF] [--defaults-group=GROUP-NAME] BACKUP-DIR\n\
\n\
innobackupex --move-back [--defaults-file=MY.CNF] [--defaults-group=GROUP-NAME] BACKUP-DIR\n\
\n\
innobackupex [--decompress]\n\
             [--parallel=NUMBER-OF-FORKS] BACKUP-DIR\n\
\n\
DESCRIPTION\n\
\n\
The first command line above makes a hot backup of a database.\n\
By default it creates a backup directory (named by the current date\n\
	and time) in the given backup root directory.  With the --no-timestamp\n\
option it does not create a time-stamped backup directory, but it puts\n\
the backup in the given directory (which must not exist).  This\n\
command makes a complete backup of all MyISAM and InnoDB tables and\n\
indexes in all databases or in all of the databases specified with the\n\
--databases option.  The created backup contains .frm, .MRG, .MYD,\n\
.MYI, .MAD, .MAI, .TRG, .TRN, .ARM, .ARZ, .CSM, CSV, .opt, .par, and\n\
InnoDB data and log files.  The MY.CNF options file defines the\n\
location of the database.\n\
\n\
The --apply-log command prepares a backup for starting a MySQL\n\
server on the backup. This command recovers InnoDB data files as specified\n\
in BACKUP-DIR/backup-my.cnf using BACKUP-DIR/ib_logfile0,\n\
and creates new InnoDB log files as specified in BACKUP-DIR/backup-my.cnf.\n\
The BACKUP-DIR should be the path to a backup directory\n\
\n\
The --copy-back command copies data, index, and log files\n\
from the backup directory back to their original locations.\n\
The MY.CNF options file defines the original location of the database.\n\
The BACKUP-DIR is the path to a backup directory.\n\
\n\
The --move-back command is similar to --copy-back with the only difference that\n\
it moves files to their original locations rather than copies them. As this\n\
option removes backup files, it must be used with caution. It may be useful in\n\
cases when there is not enough free disk space to copy files.\n\
\n\
The --decompress command will decompress a backup made\n\
with the --compress option. The\n\
--parallel option will allow multiple files to be decompressed\n\
simultaneously. In order to decompress, the qpress utility MUST be installed\n\
and accessable within the path. This process will remove the original\n\
compressed files and leave the results in the same location.\n\
\n\
On success the exit code innobackupex is 0. A non-zero exit code \n\
indicates an error.\n");
	printf("Usage: [%s [--defaults-file=#] --backup | %s [--defaults-file=#] --prepare] [OPTIONS]\n", my_progname, my_progname);
	my_print_help(ibx_long_options);
}


static
my_bool
ibx_get_one_option(int optid,
		const struct my_option *opt __attribute__((unused)),
		char *argument)
{
	switch(optid) {
	case '?':
		usage();
		exit(0);
		break;
	case 'v':
		printf("innobackupex version %s %s (%s)",
			MYSQL_SERVER_VERSION,
			SYSTEM_TYPE, MACHINE_TYPE);
		exit(0);
		break;
	case OPT_HISTORY:
		if (argument) {
			opt_ibx_history = argument;
		} else {
			opt_ibx_history = "";
		}
		break;
	case OPT_STREAM:
		if (!strcasecmp(argument, "mbstream") ||
		    !strcasecmp(argument, "xbstream"))
			xtrabackup_stream_fmt = XB_STREAM_FMT_XBSTREAM;
		else {
			ibx_msg("Invalid --stream argument: %s\n", argument);
			return 1;
		}
		xtrabackup_stream = TRUE;
		break;
	case OPT_COMPRESS:
		if (argument == NULL)
			xtrabackup_compress_alg = "quicklz";
		else if (strcasecmp(argument, "quicklz"))
		{
			ibx_msg("Invalid --compress argument: %s\n", argument);
			return 1;
		}
		xtrabackup_compress = TRUE;
		break;
	case 'p':
		if (argument)
		{
		        char *start = argument;
			my_free(opt_ibx_password);
			opt_ibx_password= my_strdup(argument, MYF(MY_FAE));
			/*  Destroy argument */
			while (*argument)
				*argument++= 'x';
			if (*start)
				start[1]=0 ;
		}
		break;
        }
	return(0);
}

bool
make_backup_dir()
{
	time_t t = time(NULL);
	char buf[100];

	if (!opt_ibx_notimestamp && !ibx_xtrabackup_stream_str) {
		strftime(buf, sizeof(buf), "%Y-%m-%d_%H-%M-%S", localtime(&t));
		ut_a(asprintf(&ibx_backup_directory, "%s/%s",
				ibx_position_arg, buf) != -1);
	} else {
		ibx_backup_directory = strdup(ibx_position_arg);
	}

	if (!directory_exists(ibx_backup_directory, true)) {
		return(false);
	}

	return(true);
}

bool
ibx_handle_options(int *argc, char ***argv)
{
	int i, n_arguments;

	if (handle_options(argc, argv, ibx_long_options, ibx_get_one_option)) {
		return(false);
	}

	if (opt_ibx_apply_log) {
		ibx_mode = IBX_MODE_APPLY_LOG;
	} else if (opt_ibx_copy_back) {
		ibx_mode = IBX_MODE_COPY_BACK;
	} else if (opt_ibx_move_back) {
		ibx_mode = IBX_MODE_MOVE_BACK;
	} else if (opt_ibx_decompress) {
		ibx_mode = IBX_MODE_DECRYPT_DECOMPRESS;
	} else {
		ibx_mode = IBX_MODE_BACKUP;
	}

	/* find and save position argument */
	i = 0;
	n_arguments = 0;
	while (i < *argc) {
		char *opt = (*argv)[i];

		if (strncmp(opt, "--", 2) != 0
		    && !(strlen(opt) == 2 && opt[0] == '-')) {
			if (ibx_position_arg != NULL
				&& ibx_position_arg != opt) {
				ibx_msg("Error: extra argument found %s\n",
					opt);
			}
			ibx_position_arg = opt;
			++n_arguments;
		}
		++i;
	}

	*argc -= n_arguments;
	if (n_arguments > 1) {
		return(false);
	}

	if (ibx_position_arg == NULL) {
		ibx_msg("Missing argument\n");
		return(false);
	}

	/* set argv[0] to be the program name */
	--(*argv);
	++(*argc);

	return(true);
}

/*********************************************************************//**
Parse command-line options, connect to MySQL server,
detect server capabilities, etc.
@return	true on success. */
bool
ibx_init()
{
	const char *run;

	/*=====================*/
	xtrabackup_copy_back = opt_ibx_copy_back;
	xtrabackup_move_back = opt_ibx_move_back;
	opt_galera_info = opt_ibx_galera_info;
	opt_slave_info = opt_ibx_slave_info;
	opt_no_lock = opt_ibx_no_lock;
	opt_safe_slave_backup = opt_ibx_safe_slave_backup;
	opt_rsync = opt_ibx_rsync;
	opt_force_non_empty_dirs = opt_ibx_force_non_empty_dirs;
	opt_noversioncheck = opt_ibx_noversioncheck;
	opt_no_backup_locks = opt_ibx_no_backup_locks;
	opt_decompress = opt_ibx_decompress;

	opt_incremental_history_name = opt_ibx_incremental_history_name;
	opt_incremental_history_uuid = opt_ibx_incremental_history_uuid;

	opt_user = opt_ibx_user;
	opt_password = opt_ibx_password;
	opt_host = opt_ibx_host;
	opt_defaults_group = opt_ibx_defaults_group;
	opt_socket = opt_ibx_socket;
	opt_port = opt_ibx_port;

	opt_lock_wait_query_type = opt_ibx_lock_wait_query_type;
	opt_kill_long_query_type = opt_ibx_kill_long_query_type;

	opt_kill_long_queries_timeout = opt_ibx_kill_long_queries_timeout;
	opt_lock_wait_timeout = opt_ibx_lock_wait_timeout;
	opt_lock_wait_threshold = opt_ibx_lock_wait_threshold;
	opt_debug_sleep_before_unlock = opt_ibx_debug_sleep_before_unlock;
	opt_safe_slave_backup_timeout = opt_ibx_safe_slave_backup_timeout;

	opt_history = opt_ibx_history;

	/* setup xtrabackup options */
	xb_close_files = ibx_xb_close_files;
	xtrabackup_compress_alg = ibx_xtrabackup_compress_alg;
	xtrabackup_compress_threads = ibx_xtrabackup_compress_threads;
	xtrabackup_compress_chunk_size = ibx_xtrabackup_compress_chunk_size;
	xtrabackup_export = ibx_xtrabackup_export;
	xtrabackup_extra_lsndir = ibx_xtrabackup_extra_lsndir;
	xtrabackup_incremental_basedir = ibx_xtrabackup_incremental_basedir;
	xtrabackup_incremental_dir = ibx_xtrabackup_incremental_dir;
	xtrabackup_incremental_force_scan =
					ibx_xtrabackup_incremental_force_scan;
	xtrabackup_log_copy_interval = ibx_xtrabackup_log_copy_interval;
	xtrabackup_incremental = ibx_xtrabackup_incremental;
	xtrabackup_parallel = ibx_xtrabackup_parallel;
	xtrabackup_stream_str = ibx_xtrabackup_stream_str;
	xtrabackup_tables_file = ibx_xtrabackup_tables_file;
	xtrabackup_throttle = ibx_xtrabackup_throttle;
	opt_mysql_tmpdir = ibx_opt_mysql_tmpdir;
	xtrabackup_use_memory = ibx_xtrabackup_use_memory;

	if (!opt_ibx_incremental
	    && (xtrabackup_incremental
	    	|| xtrabackup_incremental_basedir
	    	|| opt_ibx_incremental_history_name
	    	|| opt_ibx_incremental_history_uuid)) {
		ibx_msg("Error: --incremental-lsn, --incremental-basedir, "
			"--incremental-history-name and "
			"--incremental-history-uuid require the "
			"--incremental option.\n");
		return(false);
	}

	if (opt_ibx_databases != NULL) {
		if (is_path_separator(*opt_ibx_databases)) {
			xtrabackup_databases_file = opt_ibx_databases;
		} else {
			xtrabackup_databases = opt_ibx_databases;
		}
	}

	/* --tables and --tables-file options are xtrabackup only */
	ibx_partial_backup = (opt_ibx_include || opt_ibx_databases);

	if (ibx_mode == IBX_MODE_BACKUP) {

		if (!make_backup_dir()) {
			return(false);
		}
	}

	/* --binlog-info is xtrabackup only, so force
	--binlog-info=ON. i.e. behavior before the feature had been
	implemented */
	opt_binlog_info = BINLOG_INFO_ON;

	switch (ibx_mode) {
	case IBX_MODE_APPLY_LOG:
		xtrabackup_prepare = TRUE;
		xtrabackup_target_dir = ibx_position_arg;
		run = "apply-log";
		break;
	case IBX_MODE_BACKUP:
		xtrabackup_backup = TRUE;
		xtrabackup_target_dir = ibx_backup_directory;
		if (opt_ibx_include != NULL) {
			xtrabackup_tables = opt_ibx_include;
		}
		run = "backup";
		break;
	case IBX_MODE_COPY_BACK:
		xtrabackup_copy_back = TRUE;
		xtrabackup_target_dir = ibx_position_arg;
		run = "copy-back";
		break;
	case IBX_MODE_MOVE_BACK:
		xtrabackup_move_back = TRUE;
		xtrabackup_target_dir = ibx_position_arg;
		run = "move-back";
		break;
	case IBX_MODE_DECRYPT_DECOMPRESS:
		xtrabackup_decrypt_decompress = TRUE;
		xtrabackup_target_dir = ibx_position_arg;
		run = "decompress";
		break;
	default:
		ut_error;
	}

	ibx_msg("Starting the %s operation\n\n"
		"IMPORTANT: Please check that the %s run completes "
		"successfully.\n"
		"           At the end of a successful %s run innobackupex\n"
		"           prints \"completed OK!\".\n\n", run, run, run);


	return(true);
}

void
ibx_cleanup()
{
	free(ibx_backup_directory);
}
