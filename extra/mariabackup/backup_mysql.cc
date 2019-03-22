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
Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA

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
this program; if not, write to the Free Software Foundation, Inc.,
51 Franklin St, Fifth Floor, Boston, MA 02111-1301 USA

*******************************************************/
#define MYSQL_CLIENT

#include <my_global.h>
#include <mysql.h>
#include <mysqld.h>
#include <my_sys.h>
#include <string.h>
#include <limits>
#include "common.h"
#include "xtrabackup.h"
#include "srv0srv.h"
#include "mysql_version.h"
#include "backup_copy.h"
#include "backup_mysql.h"
#include "mysqld.h"
#include "encryption_plugin.h"
#include <sstream>
#include <sql_error.h>
#include "page0zip.h"

char *tool_name;
char tool_args[2048];

/* mysql flavor and version */
mysql_flavor_t server_flavor = FLAVOR_UNKNOWN;
unsigned long mysql_server_version = 0;

/* server capabilities */
bool have_changed_page_bitmaps = false;
bool have_backup_locks = false;
bool have_lock_wait_timeout = false;
bool have_galera_enabled = false;
bool have_flush_engine_logs = false;
bool have_multi_threaded_slave = false;
bool have_gtid_slave = false;

/* Kill long selects */
os_thread_id_t	kill_query_thread_id;
os_event_t	kill_query_thread_started;
os_event_t	kill_query_thread_stopped;
os_event_t	kill_query_thread_stop;

bool sql_thread_started = false;
char *mysql_slave_position = NULL;
char *mysql_binlog_position = NULL;
char *buffer_pool_filename = NULL;

/* History on server */
time_t history_start_time;
time_t history_end_time;
time_t history_lock_time;

MYSQL *mysql_connection;

extern my_bool opt_ssl_verify_server_cert, opt_use_ssl;

MYSQL *
xb_mysql_connect()
{
	MYSQL *connection = mysql_init(NULL);
	char mysql_port_str[std::numeric_limits<int>::digits10 + 3];

	sprintf(mysql_port_str, "%d", opt_port);

	if (connection == NULL) {
		msg("Failed to init MySQL struct: %s.",
			mysql_error(connection));
		return(NULL);
	}

	if (!opt_secure_auth) {
		mysql_options(connection, MYSQL_SECURE_AUTH,
			      (char *) &opt_secure_auth);
	}

	if (xb_plugin_dir && *xb_plugin_dir){
		mysql_options(connection, MYSQL_PLUGIN_DIR, xb_plugin_dir);
	}
	mysql_options(connection, MYSQL_OPT_PROTOCOL, &opt_protocol);
	mysql_options(connection,MYSQL_SET_CHARSET_NAME, "utf8");

	msg("Connecting to MySQL server host: %s, user: %s, password: %s, "
	       "port: %s, socket: %s", opt_host ? opt_host : "localhost",
	       opt_user ? opt_user : "not set",
	       opt_password ? "set" : "not set",
	       opt_port != 0 ? mysql_port_str : "not set",
	       opt_socket ? opt_socket : "not set");

#ifdef HAVE_OPENSSL
	if (opt_use_ssl)
	{
		mysql_ssl_set(connection, opt_ssl_key, opt_ssl_cert,
			      opt_ssl_ca, opt_ssl_capath,
			      opt_ssl_cipher);
		mysql_options(connection, MYSQL_OPT_SSL_CRL, opt_ssl_crl);
		mysql_options(connection, MYSQL_OPT_SSL_CRLPATH,
			      opt_ssl_crlpath);
	}
	mysql_options(connection,MYSQL_OPT_SSL_VERIFY_SERVER_CERT,
		      (char*)&opt_ssl_verify_server_cert);
#endif

	if (!mysql_real_connect(connection,
				opt_host ? opt_host : "localhost",
				opt_user,
				opt_password,
				"" /*database*/, opt_port,
				opt_socket, 0)) {
		msg("Failed to connect to MySQL server: %s.", mysql_error(connection));
		mysql_close(connection);
		return(NULL);
	}

	xb_mysql_query(connection, "SET SESSION wait_timeout=2147483, max_statement_time=0",
		       false, true);

	return(connection);
}

/*********************************************************************//**
Execute mysql query. */
MYSQL_RES *
xb_mysql_query(MYSQL *connection, const char *query, bool use_result,
		bool die_on_error)
{
	MYSQL_RES *mysql_result = NULL;

	if (mysql_query(connection, query)) {
		if (die_on_error) {
			die("failed to execute query %s: %s", query, mysql_error(connection));
		} else {
			msg("Error: failed to execute query %s: %s", query, mysql_error(connection));
		}
		return(NULL);
	}

	/* store result set on client if there is a result */
	if (mysql_field_count(connection) > 0) {
		if ((mysql_result = mysql_store_result(connection)) == NULL) {
			die("failed to fetch query result %s: %s",
				query, mysql_error(connection));
		}

		if (!use_result) {
			mysql_free_result(mysql_result);
			mysql_result = NULL;
		}
	}

	return mysql_result;
}


struct mysql_variable {
	const char *name;
	char **value;
};


static
void
read_mysql_variables(MYSQL *connection, const char *query, mysql_variable *vars,
	bool vertical_result)
{
	MYSQL_RES *mysql_result;
	MYSQL_ROW row;
	mysql_variable *var;

	mysql_result = xb_mysql_query(connection, query, true);

	ut_ad(!vertical_result || mysql_num_fields(mysql_result) == 2);

	if (vertical_result) {
		while ((row = mysql_fetch_row(mysql_result))) {
			char *name = row[0];
			char *value = row[1];
			for (var = vars; var->name; var++) {
				if (strcmp(var->name, name) == 0
				    && value != NULL) {
					*(var->value) = strdup(value);
				}
			}
		}
	} else {
		MYSQL_FIELD *field;

		if ((row = mysql_fetch_row(mysql_result)) != NULL) {
			int i = 0;
			while ((field = mysql_fetch_field(mysql_result))
				!= NULL) {
				char *name = field->name;
				char *value = row[i];
				for (var = vars; var->name; var++) {
					if (strcmp(var->name, name) == 0
					    && value != NULL) {
						*(var->value) = strdup(value);
					}
				}
				++i;
			}
		}
	}

	mysql_free_result(mysql_result);
}


static
void
free_mysql_variables(mysql_variable *vars)
{
	mysql_variable *var;

	for (var = vars; var->name; var++) {
		free(*(var->value));
	}
}


static
char *
read_mysql_one_value(MYSQL *connection, const char *query)
{
	MYSQL_RES *mysql_result;
	MYSQL_ROW row;
	char *result = NULL;

	mysql_result = xb_mysql_query(connection, query, true);

	ut_ad(mysql_num_fields(mysql_result) == 1);

	if ((row = mysql_fetch_row(mysql_result))) {
		result = strdup(row[0]);
	}

	mysql_free_result(mysql_result);

	return(result);
}

static
bool
check_server_version(unsigned long version_number,
		     const char *version_string,
		     const char *version_comment,
		     const char *innodb_version)
{
	bool version_supported = false;
	bool mysql51 = false;

	mysql_server_version = version_number;

	server_flavor = FLAVOR_UNKNOWN;
	if (strstr(version_comment, "Percona") != NULL) {
		server_flavor = FLAVOR_PERCONA_SERVER;
	} else if (strstr(version_comment, "MariaDB") != NULL ||
		   strstr(version_string, "MariaDB") != NULL) {
		server_flavor = FLAVOR_MARIADB;
	} else if (strstr(version_comment, "MySQL") != NULL) {
		server_flavor = FLAVOR_MYSQL;
	}

	mysql51 = version_number > 50100 && version_number < 50500;
	version_supported = version_supported
		|| (mysql51 && innodb_version != NULL);
	version_supported = version_supported
		|| (version_number > 50500 && version_number < 50700);
	version_supported = version_supported
		|| ((version_number > 100000)
		    && server_flavor == FLAVOR_MARIADB);

	if (mysql51 && innodb_version == NULL) {
		msg("Error: Built-in InnoDB in MySQL 5.1 is not "
		    "supported in this release. You can either use "
		    "Percona XtraBackup 2.0, or upgrade to InnoDB "
		    "plugin.");
	} else if (!version_supported) {
		msg("Error: Unsupported server version: '%s'. Please "
		    "report a bug at "
		    "https://bugs.launchpad.net/percona-xtrabackup",
		    version_string);
	}

	return(version_supported);
}

/*********************************************************************//**
Receive options important for XtraBackup from MySQL server.
@return	true on success. */
bool
get_mysql_vars(MYSQL *connection)
{
	char *gtid_mode_var = NULL;
	char *version_var = NULL;
	char *version_comment_var = NULL;
	char *innodb_version_var = NULL;
	char *have_backup_locks_var = NULL;
	char *log_bin_var = NULL;
	char *lock_wait_timeout_var= NULL;
	char *wsrep_on_var = NULL;
	char *slave_parallel_workers_var = NULL;
	char *gtid_slave_pos_var = NULL;
	char *innodb_buffer_pool_filename_var = NULL;
	char *datadir_var = NULL;
	char *innodb_log_group_home_dir_var = NULL;
	char *innodb_log_file_size_var = NULL;
	char *innodb_log_files_in_group_var = NULL;
	char *innodb_data_file_path_var = NULL;
	char *innodb_data_home_dir_var = NULL;
	char *innodb_undo_directory_var = NULL;
	char *innodb_page_size_var = NULL;
	char *innodb_undo_tablespaces_var = NULL;
	char *page_zip_level_var = NULL;
	char *endptr;
	unsigned long server_version = mysql_get_server_version(connection);

	bool ret = true;

	mysql_variable mysql_vars[] = {
		{"have_backup_locks", &have_backup_locks_var},
		{"log_bin", &log_bin_var},
		{"lock_wait_timeout", &lock_wait_timeout_var},
		{"gtid_mode", &gtid_mode_var},
		{"version", &version_var},
		{"version_comment", &version_comment_var},
		{"innodb_version", &innodb_version_var},
		{"wsrep_on", &wsrep_on_var},
		{"slave_parallel_workers", &slave_parallel_workers_var},
		{"gtid_slave_pos", &gtid_slave_pos_var},
		{"innodb_buffer_pool_filename",
			&innodb_buffer_pool_filename_var},
		{"datadir", &datadir_var},
		{"innodb_log_group_home_dir", &innodb_log_group_home_dir_var},
		{"innodb_log_file_size", &innodb_log_file_size_var},
		{"innodb_log_files_in_group", &innodb_log_files_in_group_var},
		{"innodb_data_file_path", &innodb_data_file_path_var},
		{"innodb_data_home_dir", &innodb_data_home_dir_var},
		{"innodb_undo_directory", &innodb_undo_directory_var},
		{"innodb_page_size", &innodb_page_size_var},
		{"innodb_undo_tablespaces", &innodb_undo_tablespaces_var},
		{"innodb_compression_level", &page_zip_level_var},
		{NULL, NULL}
	};

	read_mysql_variables(connection, "SHOW VARIABLES",
				mysql_vars, true);

	if (have_backup_locks_var != NULL && !opt_no_backup_locks) {
		have_backup_locks = true;
	}

	if (opt_binlog_info == BINLOG_INFO_AUTO) {
		if (log_bin_var != NULL && !strcmp(log_bin_var, "ON"))
			opt_binlog_info = BINLOG_INFO_ON;
		else
			opt_binlog_info = BINLOG_INFO_OFF;
	}

	if (lock_wait_timeout_var != NULL) {
		have_lock_wait_timeout = true;
	}

	if (wsrep_on_var != NULL) {
		have_galera_enabled = true;
	}

	/* Check server version compatibility and detect server flavor */

	if (!(ret = check_server_version(server_version, version_var,
					 version_comment_var,
					 innodb_version_var))) {
		goto out;
	}

	if (server_version > 50500) {
		have_flush_engine_logs = true;
	}

	if (slave_parallel_workers_var != NULL
		&& atoi(slave_parallel_workers_var) > 0) {
		have_multi_threaded_slave = true;
	}

	if (innodb_buffer_pool_filename_var != NULL) {
		buffer_pool_filename = strdup(innodb_buffer_pool_filename_var);
	}

	if ((gtid_mode_var && strcmp(gtid_mode_var, "ON") == 0) ||
	    (gtid_slave_pos_var && *gtid_slave_pos_var)) {
		have_gtid_slave = true;
	}

	msg("Using server version %s", version_var);

	if (!(ret = detect_mysql_capabilities_for_backup())) {
		goto out;
	}

	/* make sure datadir value is the same in configuration file */
	if (check_if_param_set("datadir")) {
		if (!directory_exists(mysql_data_home, false)) {
			msg("Warning: option 'datadir' points to "
			    "nonexistent directory '%s'", mysql_data_home);
		}
		if (!directory_exists(datadir_var, false)) {
			msg("Warning: MySQL variable 'datadir' points to "
			    "nonexistent directory '%s'", datadir_var);
		}
		if (!equal_paths(mysql_data_home, datadir_var)) {
			msg("Warning: option 'datadir' has different "
				"values:\n"
				"  '%s' in defaults file\n"
				"  '%s' in SHOW VARIABLES",
				mysql_data_home, datadir_var);
		}
	}

	/* get some default values is they are missing from my.cnf */
	if (datadir_var && *datadir_var) {
		strmake(mysql_real_data_home, datadir_var, FN_REFLEN - 1);
		mysql_data_home= mysql_real_data_home;
	}

	if (innodb_data_file_path_var && *innodb_data_file_path_var) {
		innobase_data_file_path = my_strdup(
			innodb_data_file_path_var, MYF(MY_FAE));
	}

	if (innodb_data_home_dir_var) {
		innobase_data_home_dir = my_strdup(
			innodb_data_home_dir_var, MYF(MY_FAE));
	}

	if (innodb_log_group_home_dir_var
	    && *innodb_log_group_home_dir_var) {
		srv_log_group_home_dir = my_strdup(
			innodb_log_group_home_dir_var, MYF(MY_FAE));
	}

	if (innodb_undo_directory_var && *innodb_undo_directory_var) {
		srv_undo_dir = my_strdup(
			innodb_undo_directory_var, MYF(MY_FAE));
	}

	if (innodb_log_files_in_group_var) {
		srv_n_log_files = strtol(
			innodb_log_files_in_group_var, &endptr, 10);
		ut_ad(*endptr == 0);
	}

	if (innodb_log_file_size_var) {
		srv_log_file_size = strtoll(
			innodb_log_file_size_var, &endptr, 10);
		ut_ad(*endptr == 0);
	}

	if (innodb_page_size_var) {
		innobase_page_size = strtoll(
			innodb_page_size_var, &endptr, 10);
		ut_ad(*endptr == 0);
	}

	if (innodb_undo_tablespaces_var) {
		srv_undo_tablespaces = strtoul(innodb_undo_tablespaces_var,
					       &endptr, 10);
		ut_ad(*endptr == 0);
	}

	if (page_zip_level_var != NULL) {
		page_zip_level = strtoul(page_zip_level_var, &endptr, 10);
		ut_ad(*endptr == 0);
	}

out:
	free_mysql_variables(mysql_vars);

	return(ret);
}

/*********************************************************************//**
Query the server to find out what backup capabilities it supports.
@return	true on success. */
bool
detect_mysql_capabilities_for_backup()
{
	const char *query = "SELECT 'INNODB_CHANGED_PAGES', COUNT(*) FROM "
				"INFORMATION_SCHEMA.PLUGINS "
			    "WHERE PLUGIN_NAME LIKE 'INNODB_CHANGED_PAGES'";
	char *innodb_changed_pages = NULL;
	mysql_variable vars[] = {
		{"INNODB_CHANGED_PAGES", &innodb_changed_pages}, {NULL, NULL}};

	if (xtrabackup_incremental) {

		read_mysql_variables(mysql_connection, query, vars, true);

		ut_ad(innodb_changed_pages != NULL);

		have_changed_page_bitmaps = (atoi(innodb_changed_pages) == 1);

		/* INNODB_CHANGED_PAGES are listed in
		INFORMATION_SCHEMA.PLUGINS in MariaDB, but
		FLUSH NO_WRITE_TO_BINLOG CHANGED_PAGE_BITMAPS
		is not supported for versions below 10.1.6
		(see MDEV-7472) */
		if (server_flavor == FLAVOR_MARIADB &&
		    mysql_server_version < 100106) {
			have_changed_page_bitmaps = false;
		}

		free_mysql_variables(vars);
	}

	/* do some sanity checks */
	if (opt_galera_info && !have_galera_enabled) {
		msg("--galera-info is specified on the command "
		 	"line, but the server does not support Galera "
		 	"replication. Ignoring the option.");
		opt_galera_info = false;
	}

	if (opt_slave_info && have_multi_threaded_slave &&
	    !have_gtid_slave) {
	    	msg("The --slave-info option requires GTID enabled for a "
			"multi-threaded slave.");
		return(false);
	}

	return(true);
}

static
bool
select_incremental_lsn_from_history(lsn_t *incremental_lsn)
{
	MYSQL_RES *mysql_result;
	char query[1000];
	char buf[100];

	if (opt_incremental_history_name) {
		mysql_real_escape_string(mysql_connection, buf,
				opt_incremental_history_name,
				(unsigned long)strlen(opt_incremental_history_name));
		snprintf(query, sizeof(query),
			"SELECT innodb_to_lsn "
			"FROM PERCONA_SCHEMA.xtrabackup_history "
			"WHERE name = '%s' "
			"AND innodb_to_lsn IS NOT NULL "
			"ORDER BY innodb_to_lsn DESC LIMIT 1",
			buf);
	}

	if (opt_incremental_history_uuid) {
		mysql_real_escape_string(mysql_connection, buf,
				opt_incremental_history_uuid,
				(unsigned long)strlen(opt_incremental_history_uuid));
		snprintf(query, sizeof(query),
			"SELECT innodb_to_lsn "
			"FROM PERCONA_SCHEMA.xtrabackup_history "
			"WHERE uuid = '%s' "
			"AND innodb_to_lsn IS NOT NULL "
			"ORDER BY innodb_to_lsn DESC LIMIT 1",
			buf);
	}

	mysql_result = xb_mysql_query(mysql_connection, query, true);

	ut_ad(mysql_num_fields(mysql_result) == 1);
	const MYSQL_ROW row = mysql_fetch_row(mysql_result);
	if (row) {
		*incremental_lsn = strtoull(row[0], NULL, 10);
		msg("Found and using lsn: " LSN_PF " for %s %s",
		    *incremental_lsn,
		    opt_incremental_history_uuid ? "uuid" : "name",
		    opt_incremental_history_uuid ?
		    opt_incremental_history_uuid :
		    opt_incremental_history_name);
	} else {
		msg("Error while attempting to find history record "
			"for %s %s",
			opt_incremental_history_uuid ? "uuid" : "name",
			opt_incremental_history_uuid ?
		    		opt_incremental_history_uuid :
		    		opt_incremental_history_name);
	}

	mysql_free_result(mysql_result);

	return(row != NULL);
}

static
const char *
eat_sql_whitespace(const char *query)
{
	bool comment = false;

	while (*query) {
		if (comment) {
			if (query[0] == '*' && query[1] == '/') {
				query += 2;
				comment = false;
				continue;
			}
			++query;
			continue;
		}
		if (query[0] == '/' && query[1] == '*') {
			query += 2;
			comment = true;
			continue;
		}
		if (strchr("\t\n\r (", query[0])) {
			++query;
			continue;
		}
		break;
	}

	return(query);
}

static
bool
is_query_from_list(const char *query, const char **list)
{
	const char **item;

	query = eat_sql_whitespace(query);

	item = list;
	while (*item) {
		if (strncasecmp(query, *item, strlen(*item)) == 0) {
			return(true);
		}
		++item;
	}

	return(false);
}

static
bool
is_query(const char *query)
{
	const char *query_list[] = {"insert", "update", "delete", "replace",
		"alter", "load", "select", "do", "handler", "call", "execute",
		"begin", NULL};

	return is_query_from_list(query, query_list);
}

static
bool
is_select_query(const char *query)
{
	const char *query_list[] = {"select", NULL};

	return is_query_from_list(query, query_list);
}

static
bool
is_update_query(const char *query)
{
	const char *query_list[] = {"insert", "update", "delete", "replace",
					"alter", "load", NULL};

	return is_query_from_list(query, query_list);
}

static
bool
have_queries_to_wait_for(MYSQL *connection, uint threshold)
{
	MYSQL_RES *result = xb_mysql_query(connection, "SHOW FULL PROCESSLIST",
					   true);
	const bool all_queries = (opt_lock_wait_query_type == QUERY_TYPE_ALL);
	bool have_to_wait = false;

	while (MYSQL_ROW row = mysql_fetch_row(result)) {
		const char	*info		= row[7];
		int		duration	= row[5] ? atoi(row[5]) : 0;
		char		*id		= row[0];

		if (info != NULL
		    && duration >= (int)threshold
		    && ((all_queries && is_query(info))
		    	|| is_update_query(info))) {
			msg("Waiting for query %s (duration %d sec): %s",
			       id, duration, info);
			have_to_wait = true;
			break;
		}
	}

	mysql_free_result(result);
	return(have_to_wait);
}

static
void
kill_long_queries(MYSQL *connection, time_t timeout)
{
	char kill_stmt[100];

	MYSQL_RES *result = xb_mysql_query(connection, "SHOW FULL PROCESSLIST",
					   true);
	const bool all_queries = (opt_kill_long_query_type == QUERY_TYPE_ALL);
	while (MYSQL_ROW row = mysql_fetch_row(result)) {
		const char	*info		= row[7];
		long long		duration	= row[5]? atoll(row[5]) : 0;
		char		*id		= row[0];

		if (info != NULL &&
		    (time_t)duration >= timeout &&
		    ((all_queries && is_query(info)) ||
		    	is_select_query(info))) {
			msg("Killing query %s (duration %d sec): %s",
			       id, (int)duration, info);
			snprintf(kill_stmt, sizeof(kill_stmt),
				    "KILL %s", id);
			xb_mysql_query(connection, kill_stmt, false, false);
		}
	}

	mysql_free_result(result);
}

static
bool
wait_for_no_updates(MYSQL *connection, uint timeout, uint threshold)
{
	time_t	start_time;

	start_time = time(NULL);

	msg("Waiting %u seconds for queries running longer than %u seconds "
	       "to finish", timeout, threshold);

	while (time(NULL) <= (time_t)(start_time + timeout)) {
		if (!have_queries_to_wait_for(connection, threshold)) {
			return(true);
		}
		os_thread_sleep(1000000);
	}

	msg("Unable to obtain lock. Please try again later.");

	return(false);
}

static
os_thread_ret_t
DECLARE_THREAD(kill_query_thread)(
/*===============*/
	void *arg __attribute__((unused)))
{
	MYSQL	*mysql;
	time_t	start_time;

	start_time = time(NULL);

	os_event_set(kill_query_thread_started);

	msg("Kill query timeout %d seconds.",
	       opt_kill_long_queries_timeout);

	while (time(NULL) - start_time <
				(time_t)opt_kill_long_queries_timeout) {
		if (os_event_wait_time(kill_query_thread_stop, 1000) !=
		    OS_SYNC_TIME_EXCEEDED) {
			goto stop_thread;
		}
	}

	if ((mysql = xb_mysql_connect()) == NULL) {
		msg("Error: kill query thread failed");
		goto stop_thread;
	}

	while (true) {
		kill_long_queries(mysql, time(NULL) - start_time);
		if (os_event_wait_time(kill_query_thread_stop, 1000) !=
		    OS_SYNC_TIME_EXCEEDED) {
			break;
		}
	}

	mysql_close(mysql);

stop_thread:
	msg("Kill query thread stopped");

	os_event_set(kill_query_thread_stopped);

	os_thread_exit();
	OS_THREAD_DUMMY_RETURN;
}


static
void
start_query_killer()
{
	kill_query_thread_stop		= os_event_create(0);
	kill_query_thread_started	= os_event_create(0);
	kill_query_thread_stopped	= os_event_create(0);

	os_thread_create(kill_query_thread, NULL, &kill_query_thread_id);

	os_event_wait(kill_query_thread_started);
}

static
void
stop_query_killer()
{
	os_event_set(kill_query_thread_stop);
	os_event_wait_time(kill_query_thread_stopped, 60000);
}


/*********************************************************************//**
Function acquires either a backup tables lock, if supported
by the server, or a global read lock (FLUSH TABLES WITH READ LOCK)
otherwise.
@returns true if lock acquired */
bool
lock_tables(MYSQL *connection)
{
	if (have_lock_wait_timeout) {
		/* Set the maximum supported session value for
		lock_wait_timeout to prevent unnecessary timeouts when the
		global value is changed from the default */
		xb_mysql_query(connection,
			"SET SESSION lock_wait_timeout=31536000", false);
	}

	if (have_backup_locks) {
		msg("Executing LOCK TABLES FOR BACKUP...");
		xb_mysql_query(connection, "LOCK TABLES FOR BACKUP", false);
		return(true);
	}

	if (opt_lock_wait_timeout) {
		if (!wait_for_no_updates(connection, opt_lock_wait_timeout,
					 opt_lock_wait_threshold)) {
			return(false);
		}
	}

	msg("Acquiring BACKUP LOCKS...");

	if (opt_kill_long_queries_timeout) {
		start_query_killer();
	}

	if (have_galera_enabled) {
		xb_mysql_query(connection,
				"SET SESSION wsrep_causal_reads=0", false);
	}

	xb_mysql_query(connection, "BACKUP STAGE START", true);
	//xb_mysql_query(connection, "BACKUP STAGE FLUSH", true);
	//xb_mysql_query(connection, "BACKUP STAGE BLOCK_DDL", true);
	xb_mysql_query(connection, "BACKUP STAGE BLOCK_COMMIT", true);

	if (opt_kill_long_queries_timeout) {
		stop_query_killer();
	}

	return(true);
}


/*********************************************************************//**
If backup locks are used, execute LOCK BINLOG FOR BACKUP provided that we are
not in the --no-lock mode and the lock has not been acquired already.
@returns true if lock acquired */
bool
lock_binlog_maybe(MYSQL *connection)
{
	if (have_backup_locks && !opt_no_lock && !binlog_locked) {
		msg("Executing LOCK BINLOG FOR BACKUP...");
		xb_mysql_query(connection, "LOCK BINLOG FOR BACKUP", false);
		binlog_locked = true;

		return(true);
	}

	return(false);
}


/*********************************************************************//**
Releases either global read lock acquired with FTWRL and the binlog
lock acquired with LOCK BINLOG FOR BACKUP, depending on
the locking strategy being used */
void
unlock_all(MYSQL *connection)
{
	if (opt_debug_sleep_before_unlock) {
		msg("Debug sleep for %u seconds",
		       opt_debug_sleep_before_unlock);
		os_thread_sleep(opt_debug_sleep_before_unlock * 1000);
	}

	msg("Executing BACKUP STAGE END");
	xb_mysql_query(connection, "BACKUP STAGE END", false);

	msg("All tables unlocked");
}


static
int
get_open_temp_tables(MYSQL *connection)
{
	char *slave_open_temp_tables = NULL;
	mysql_variable status[] = {
		{"Slave_open_temp_tables", &slave_open_temp_tables},
		{NULL, NULL}
	};
	int result = false;

	read_mysql_variables(connection,
		"SHOW STATUS LIKE 'slave_open_temp_tables'", status, true);

	result = slave_open_temp_tables ? atoi(slave_open_temp_tables) : 0;

	free_mysql_variables(status);

	return(result);
}

/*********************************************************************//**
Wait until it's safe to backup a slave.  Returns immediately if
the host isn't a slave.  Currently there's only one check:
Slave_open_temp_tables has to be zero.  Dies on timeout. */
bool
wait_for_safe_slave(MYSQL *connection)
{
	char *read_master_log_pos = NULL;
	char *slave_sql_running = NULL;
	int n_attempts = 1;
	const int sleep_time = 3;
	int open_temp_tables = 0;
	bool result = true;

	mysql_variable status[] = {
		{"Read_Master_Log_Pos", &read_master_log_pos},
		{"Slave_SQL_Running", &slave_sql_running},
		{NULL, NULL}
	};

	sql_thread_started = false;

	read_mysql_variables(connection, "SHOW SLAVE STATUS", status, false);

	if (!(read_master_log_pos && slave_sql_running)) {
		msg("Not checking slave open temp tables for "
			"--safe-slave-backup because host is not a slave");
		goto cleanup;
	}

	if (strcmp(slave_sql_running, "Yes") == 0) {
		sql_thread_started = true;
		xb_mysql_query(connection, "STOP SLAVE SQL_THREAD", false);
	}

	if (opt_safe_slave_backup_timeout > 0) {
		n_attempts = opt_safe_slave_backup_timeout / sleep_time;
	}

	open_temp_tables = get_open_temp_tables(connection);
	msg("Slave open temp tables: %d", open_temp_tables);

	while (open_temp_tables && n_attempts--) {
		msg("Starting slave SQL thread, waiting %d seconds, then "
		       "checking Slave_open_temp_tables again (%d attempts "
		       "remaining)...", sleep_time, n_attempts);

		xb_mysql_query(connection, "START SLAVE SQL_THREAD", false);
		os_thread_sleep(sleep_time * 1000000);
		xb_mysql_query(connection, "STOP SLAVE SQL_THREAD", false);

		open_temp_tables = get_open_temp_tables(connection);
		msg("Slave open temp tables: %d", open_temp_tables);
	}

	/* Restart the slave if it was running at start */
	if (open_temp_tables == 0) {
		msg("Slave is safe to backup");
		goto cleanup;
	}

	result = false;

	if (sql_thread_started) {
		msg("Restarting slave SQL thread.");
		xb_mysql_query(connection, "START SLAVE SQL_THREAD", false);
	}

	msg("Slave_open_temp_tables did not become zero after "
	       "%d seconds", opt_safe_slave_backup_timeout);

cleanup:
	free_mysql_variables(status);

	return(result);
}


/*********************************************************************//**
Retrieves MySQL binlog position of the master server in a replication
setup and saves it in a file. It also saves it in mysql_slave_position
variable. */
bool
write_slave_info(MYSQL *connection)
{
	char *master = NULL;
	char *filename = NULL;
	char *gtid_executed = NULL;
	char *position = NULL;
	char *gtid_slave_pos = NULL;
	char *ptr;
	bool result = false;

	mysql_variable status[] = {
		{"Master_Host", &master},
		{"Relay_Master_Log_File", &filename},
		{"Exec_Master_Log_Pos", &position},
		{"Executed_Gtid_Set", &gtid_executed},
		{NULL, NULL}
	};

	mysql_variable variables[] = {
		{"gtid_slave_pos", &gtid_slave_pos},
		{NULL, NULL}
	};

	read_mysql_variables(connection, "SHOW SLAVE STATUS", status, false);
	read_mysql_variables(connection, "SHOW VARIABLES", variables, true);

	if (master == NULL || filename == NULL || position == NULL) {
		msg("Failed to get master binlog coordinates "
			"from SHOW SLAVE STATUS.This means that the server is not a "
			"replication slave. Ignoring the --slave-info option");
		/* we still want to continue the backup */
		result = true;
		goto cleanup;
	}

	/* Print slave status to a file.
	If GTID mode is used, construct a CHANGE MASTER statement with
	MASTER_AUTO_POSITION and correct a gtid_purged value. */
	if (gtid_executed != NULL && *gtid_executed) {
		/* MySQL >= 5.6 with GTID enabled */

		for (ptr = strchr(gtid_executed, '\n');
		     ptr;
		     ptr = strchr(ptr, '\n')) {
			*ptr = ' ';
		}

		result = backup_file_printf(XTRABACKUP_SLAVE_INFO,
			"SET GLOBAL gtid_purged='%s';\n"
			"CHANGE MASTER TO MASTER_AUTO_POSITION=1\n",
			gtid_executed);

		ut_a(asprintf(&mysql_slave_position,
			"master host '%s', purge list '%s'",
			master, gtid_executed) != -1);
	} else if (gtid_slave_pos && *gtid_slave_pos) {
		/* MariaDB >= 10.0 with GTID enabled */
		result = backup_file_printf(XTRABACKUP_SLAVE_INFO,
			"SET GLOBAL gtid_slave_pos = '%s';\n"
			"CHANGE MASTER TO master_use_gtid = slave_pos\n",
			gtid_slave_pos);
		ut_a(asprintf(&mysql_slave_position,
			"master host '%s', gtid_slave_pos %s",
			master, gtid_slave_pos) != -1);
	} else {
		result = backup_file_printf(XTRABACKUP_SLAVE_INFO,
			"CHANGE MASTER TO MASTER_LOG_FILE='%s', "
			"MASTER_LOG_POS=%s\n", filename, position);
		ut_a(asprintf(&mysql_slave_position,
			"master host '%s', filename '%s', position '%s'",
			master, filename, position) != -1);
	}

cleanup:
	free_mysql_variables(status);
	free_mysql_variables(variables);

	return(result);
}


/*********************************************************************//**
Retrieves MySQL Galera and
saves it in a file. It also prints it to stdout. */
bool
write_galera_info(MYSQL *connection)
{
	char *state_uuid = NULL, *state_uuid55 = NULL;
	char *last_committed = NULL, *last_committed55 = NULL;
	bool result;

	mysql_variable status[] = {
		{"Wsrep_local_state_uuid", &state_uuid},
		{"wsrep_local_state_uuid", &state_uuid55},
		{"Wsrep_last_committed", &last_committed},
		{"wsrep_last_committed", &last_committed55},
		{NULL, NULL}
	};

	/* When backup locks are supported by the server, we should skip
	creating xtrabackup_galera_info file on the backup stage, because
	wsrep_local_state_uuid and wsrep_last_committed will be inconsistent
	without blocking commits. The state file will be created on the prepare
	stage using the WSREP recovery procedure. */
	if (have_backup_locks) {
		return(true);
	}

	read_mysql_variables(connection, "SHOW STATUS", status, true);

	if ((state_uuid == NULL && state_uuid55 == NULL)
		|| (last_committed == NULL && last_committed55 == NULL)) {
		msg("Failed to get master wsrep state from SHOW STATUS.");
		result = false;
		goto cleanup;
	}

	result = backup_file_printf(XTRABACKUP_GALERA_INFO,
		"%s:%s\n", state_uuid ? state_uuid : state_uuid55,
			last_committed ? last_committed : last_committed55);

cleanup:
	free_mysql_variables(status);

	return(result);
}


/*********************************************************************//**
Flush and copy the current binary log file into the backup,
if GTID is enabled */
bool
write_current_binlog_file(MYSQL *connection)
{
	char *executed_gtid_set = NULL;
	char *gtid_binlog_state = NULL;
	char *log_bin_file = NULL;
	char *log_bin_dir = NULL;
	bool gtid_exists;
	bool result = true;
	char filepath[FN_REFLEN];

	mysql_variable status[] = {
		{"Executed_Gtid_Set", &executed_gtid_set},
		{NULL, NULL}
	};

	mysql_variable status_after_flush[] = {
		{"File", &log_bin_file},
		{NULL, NULL}
	};

	mysql_variable vars[] = {
		{"gtid_binlog_state", &gtid_binlog_state},
		{"log_bin_basename", &log_bin_dir},
		{NULL, NULL}
	};

	read_mysql_variables(connection, "SHOW MASTER STATUS", status, false);
	read_mysql_variables(connection, "SHOW VARIABLES", vars, true);

	gtid_exists = (executed_gtid_set && *executed_gtid_set)
			|| (gtid_binlog_state && *gtid_binlog_state);

	if (gtid_exists) {
		size_t log_bin_dir_length;

		lock_binlog_maybe(connection);

		xb_mysql_query(connection, "FLUSH BINARY LOGS", false);

		read_mysql_variables(connection, "SHOW MASTER STATUS",
			status_after_flush, false);

		if (opt_log_bin != NULL && strchr(opt_log_bin, FN_LIBCHAR)) {
			/* If log_bin is set, it has priority */
			if (log_bin_dir) {
				free(log_bin_dir);
			}
			log_bin_dir = strdup(opt_log_bin);
		} else if (log_bin_dir == NULL) {
			/* Default location is MySQL datadir */
			log_bin_dir = strdup("./");
		}

		dirname_part(log_bin_dir, log_bin_dir, &log_bin_dir_length);

		/* strip final slash if it is not the only path component */
		if (log_bin_dir_length > 1 &&
		    log_bin_dir[log_bin_dir_length - 1] == FN_LIBCHAR) {
			log_bin_dir[log_bin_dir_length - 1] = 0;
		}

		if (log_bin_dir == NULL || log_bin_file == NULL) {
			msg("Failed to get master binlog coordinates from "
				"SHOW MASTER STATUS");
			result = false;
			goto cleanup;
		}

		snprintf(filepath, sizeof(filepath), "%s%c%s",
			 log_bin_dir, FN_LIBCHAR, log_bin_file);
		result = copy_file(ds_data, filepath, log_bin_file, 0);
	}

cleanup:
	free_mysql_variables(status_after_flush);
	free_mysql_variables(status);
	free_mysql_variables(vars);

	return(result);
}


/*********************************************************************//**
Retrieves MySQL binlog position and
saves it in a file. It also prints it to stdout. */
bool
write_binlog_info(MYSQL *connection)
{
	char *filename = NULL;
	char *position = NULL;
	char *gtid_mode = NULL;
	char *gtid_current_pos = NULL;
	char *gtid_executed = NULL;
	char *gtid = NULL;
	bool result;
	bool mysql_gtid;
	bool mariadb_gtid;

	mysql_variable status[] = {
		{"File", &filename},
		{"Position", &position},
		{"Executed_Gtid_Set", &gtid_executed},
		{NULL, NULL}
	};

	mysql_variable vars[] = {
		{"gtid_mode", &gtid_mode},
		{"gtid_current_pos", &gtid_current_pos},
		{NULL, NULL}
	};

	read_mysql_variables(connection, "SHOW MASTER STATUS", status, false);
	read_mysql_variables(connection, "SHOW VARIABLES", vars, true);

	if (filename == NULL || position == NULL) {
		/* Do not create xtrabackup_binlog_info if binary
		log is disabled */
		result = true;
		goto cleanup;
	}

	mysql_gtid = ((gtid_mode != NULL) && (strcmp(gtid_mode, "ON") == 0));
	mariadb_gtid = (gtid_current_pos != NULL);

	gtid = (gtid_executed != NULL ? gtid_executed : gtid_current_pos);

	if (mariadb_gtid || mysql_gtid) {
		ut_a(asprintf(&mysql_binlog_position,
			"filename '%s', position '%s', "
			"GTID of the last change '%s'",
			filename, position, gtid) != -1);
		result = backup_file_printf(XTRABACKUP_BINLOG_INFO,
					    "%s\t%s\t%s\n", filename, position,
					    gtid);
	} else {
		ut_a(asprintf(&mysql_binlog_position,
			"filename '%s', position '%s'",
			filename, position) != -1);
		result = backup_file_printf(XTRABACKUP_BINLOG_INFO,
					    "%s\t%s\n", filename, position);
	}

cleanup:
	free_mysql_variables(status);
	free_mysql_variables(vars);

	return(result);
}

struct escape_and_quote
{
	escape_and_quote(MYSQL *mysql, const char *str)
		: mysql(mysql), str(str) {}
	MYSQL * const mysql;
	const char * const str;
};

static
std::ostream&
operator<<(std::ostream& s, const escape_and_quote& eq)
{
	if (!eq.str)
		return s << "NULL";
	s << '\'';
	size_t len = strlen(eq.str);
	char* escaped = (char *)alloca(2 * len + 1);
	len = mysql_real_escape_string(eq.mysql, escaped, eq.str, (ulong)len);
	s << std::string(escaped, len);
	s << '\'';
	return s;
}

/*********************************************************************//**
Writes xtrabackup_info file and if backup_history is enable creates
PERCONA_SCHEMA.xtrabackup_history and writes a new history record to the
table containing all the history info particular to the just completed
backup. */
bool
write_xtrabackup_info(MYSQL *connection, const char * filename, bool history)
{

	char *uuid = NULL;
	char *server_version = NULL;
	char buf_start_time[100];
	char buf_end_time[100];
	tm tm;
	std::ostringstream oss;
	const char *xb_stream_name[] = {"file", "tar", "xbstream"};

	uuid = read_mysql_one_value(connection, "SELECT UUID()");
	server_version = read_mysql_one_value(connection, "SELECT VERSION()");
	localtime_r(&history_start_time, &tm);
	strftime(buf_start_time, sizeof(buf_start_time),
		 "%Y-%m-%d %H:%M:%S", &tm);
	history_end_time = time(NULL);
	localtime_r(&history_end_time, &tm);
	strftime(buf_end_time, sizeof(buf_end_time),
		 "%Y-%m-%d %H:%M:%S", &tm);
	bool is_partial = (xtrabackup_tables
		|| xtrabackup_tables_file
		|| xtrabackup_databases
		|| xtrabackup_databases_file
		|| xtrabackup_tables_exclude
		|| xtrabackup_databases_exclude
		);

	backup_file_printf(filename,
		"uuid = %s\n"
		"name = %s\n"
		"tool_name = %s\n"
		"tool_command = %s\n"
		"tool_version = %s\n"
		"ibbackup_version = %s\n"
		"server_version = %s\n"
		"start_time = %s\n"
		"end_time = %s\n"
		"lock_time = %d\n"
		"binlog_pos = %s\n"
		"innodb_from_lsn = %llu\n"
		"innodb_to_lsn = %llu\n"
		"partial = %s\n"
		"incremental = %s\n"
		"format = %s\n"
		"compressed = %s\n",
		uuid, /* uuid */
		opt_history ? opt_history : "",  /* name */
		tool_name,  /* tool_name */
		tool_args,  /* tool_command */
		MYSQL_SERVER_VERSION,  /* tool_version */
		MYSQL_SERVER_VERSION,  /* ibbackup_version */
		server_version,  /* server_version */
		buf_start_time,  /* start_time */
		buf_end_time,  /* end_time */
		(int)history_lock_time, /* lock_time */
		mysql_binlog_position ?
			mysql_binlog_position : "", /* binlog_pos */
		incremental_lsn, /* innodb_from_lsn */
		metadata_to_lsn, /* innodb_to_lsn */
		is_partial? "Y" : "N",
		xtrabackup_incremental ? "Y" : "N", /* incremental */
		xb_stream_name[xtrabackup_stream_fmt], /* format */
		xtrabackup_compress ? "compressed" : "N"); /* compressed */

	if (!history) {
		goto cleanup;
	}

	xb_mysql_query(connection,
		"CREATE DATABASE IF NOT EXISTS PERCONA_SCHEMA", false);
	xb_mysql_query(connection,
		"CREATE TABLE IF NOT EXISTS PERCONA_SCHEMA.xtrabackup_history("
		"uuid VARCHAR(40) NOT NULL PRIMARY KEY,"
		"name VARCHAR(255) DEFAULT NULL,"
		"tool_name VARCHAR(255) DEFAULT NULL,"
		"tool_command TEXT DEFAULT NULL,"
		"tool_version VARCHAR(255) DEFAULT NULL,"
		"ibbackup_version VARCHAR(255) DEFAULT NULL,"
		"server_version VARCHAR(255) DEFAULT NULL,"
		"start_time TIMESTAMP NULL DEFAULT NULL,"
		"end_time TIMESTAMP NULL DEFAULT NULL,"
		"lock_time BIGINT UNSIGNED DEFAULT NULL,"
		"binlog_pos VARCHAR(128) DEFAULT NULL,"
		"innodb_from_lsn BIGINT UNSIGNED DEFAULT NULL,"
		"innodb_to_lsn BIGINT UNSIGNED DEFAULT NULL,"
		"partial ENUM('Y', 'N') DEFAULT NULL,"
		"incremental ENUM('Y', 'N') DEFAULT NULL,"
		"format ENUM('file', 'tar', 'xbstream') DEFAULT NULL,"
		"compressed ENUM('Y', 'N') DEFAULT NULL"
		") CHARACTER SET utf8 ENGINE=innodb", false);


#define ESCAPE_BOOL(expr) ((expr)?"'Y'":"'N'")

	oss << "insert into PERCONA_SCHEMA.xtrabackup_history("
		<< "uuid, name, tool_name, tool_command, tool_version,"
		<< "ibbackup_version, server_version, start_time, end_time,"
		<< "lock_time, binlog_pos, innodb_from_lsn, innodb_to_lsn,"
		<< "partial, incremental, format, compressed) "
		<< "values("
		<< escape_and_quote(connection, uuid) << ","
		<< escape_and_quote(connection, opt_history) << ","
		<< escape_and_quote(connection, tool_name) << ","
		<< escape_and_quote(connection, tool_args) << ","
		<< escape_and_quote(connection, MYSQL_SERVER_VERSION) << ","
		<< escape_and_quote(connection, MYSQL_SERVER_VERSION) << ","
		<< escape_and_quote(connection, server_version) << ","
		<< "from_unixtime(" << history_start_time << "),"
		<< "from_unixtime(" << history_end_time << "),"
		<< history_lock_time << ","
		<< escape_and_quote(connection, mysql_binlog_position) << ","
		<< incremental_lsn << ","
		<< metadata_to_lsn << ","
		<< ESCAPE_BOOL(is_partial) << ","
		<< ESCAPE_BOOL(xtrabackup_incremental)<< ","
		<< escape_and_quote(connection,xb_stream_name[xtrabackup_stream_fmt]) <<","
		<< ESCAPE_BOOL(xtrabackup_compress) << ")";

	xb_mysql_query(mysql_connection, oss.str().c_str(), false);

cleanup:

	free(uuid);
	free(server_version);

	return(true);
}

extern const char *innodb_checksum_algorithm_names[];

#ifdef _WIN32
#include <algorithm>
#endif

static std::string make_local_paths(const char *data_file_path)
{
	if (strchr(data_file_path, '/') == 0
#ifdef _WIN32
		&& strchr(data_file_path, '\\') == 0
#endif
		){
		return std::string(data_file_path);
	}

	std::ostringstream buf;

	char *dup = strdup(innobase_data_file_path);
	ut_a(dup);
	char *p;
	char * token = strtok_r(dup, ";", &p);
	while (token) {
		if (buf.tellp())
			buf << ";";

		char *fname = strrchr(token, '/');
#ifdef _WIN32
		fname = std::max(fname,strrchr(token, '\\'));
#endif
		if (fname)
			buf << fname + 1;
		else
			buf << token;
		token = strtok_r(NULL, ";", &p);
	}
	free(dup);
	return buf.str();
}

bool write_backup_config_file()
{
	int rc= backup_file_printf("backup-my.cnf",
		"# This MySQL options file was generated by innobackupex.\n\n"
		"# The MySQL server\n"
		"[mysqld]\n"
		"innodb_checksum_algorithm=%s\n"
		"innodb_data_file_path=%s\n"
		"innodb_log_files_in_group=%lu\n"
		"innodb_log_file_size=%llu\n"
		"innodb_page_size=%lu\n"
		"innodb_undo_directory=%s\n"
		"innodb_undo_tablespaces=%lu\n"
		"innodb_compression_level=%u\n"
		"%s%s\n"
		"%s\n",
		innodb_checksum_algorithm_names[srv_checksum_algorithm],
		make_local_paths(innobase_data_file_path).c_str(),
		srv_n_log_files,
		srv_log_file_size,
		srv_page_size,
		srv_undo_dir,
		srv_undo_tablespaces,
		page_zip_level,
		innobase_buffer_pool_filename ?
			"innodb_buffer_pool_filename=" : "",
		innobase_buffer_pool_filename ?
			innobase_buffer_pool_filename : "",
		encryption_plugin_get_config());
		return rc;
}


static
char *make_argv(char *buf, size_t len, int argc, char **argv)
{
	size_t left= len;
	const char *arg;

	buf[0]= 0;
	++argv; --argc;
	while (argc > 0 && left > 0)
	{
		arg = *argv;
		if (strncmp(*argv, "--password", strlen("--password")) == 0) {
			arg = "--password=...";
		}
		left-= snprintf(buf + len - left, left,
				"%s%c", arg, argc > 1 ? ' ' : 0);
		++argv; --argc;
	}

	return buf;
}

void
capture_tool_command(int argc, char **argv)
{
	/* capture tool name tool args */
	tool_name = strrchr(argv[0], '/');
	tool_name = tool_name ? tool_name + 1 : argv[0];

	make_argv(tool_args, sizeof(tool_args), argc, argv);
}


bool
select_history()
{
	if (opt_incremental_history_name || opt_incremental_history_uuid) {
		if (!select_incremental_lsn_from_history(
			&incremental_lsn)) {
			return(false);
		}
	}
	return(true);
}

bool
flush_changed_page_bitmaps()
{
	if (xtrabackup_incremental && have_changed_page_bitmaps &&
	    !xtrabackup_incremental_force_scan) {
		xb_mysql_query(mysql_connection,
			"FLUSH NO_WRITE_TO_BINLOG CHANGED_PAGE_BITMAPS", false);
	}
	return(true);
}


/*********************************************************************//**
Deallocate memory, disconnect from MySQL server, etc.
@return	true on success. */
void
backup_cleanup()
{
	free(mysql_slave_position);
	free(mysql_binlog_position);
	free(buffer_pool_filename);

	if (mysql_connection) {
		mysql_close(mysql_connection);
	}
}


static MYSQL *mdl_con = NULL;

std::map<ulint, std::string> spaceid_to_tablename;

void
mdl_lock_init()
{
  mdl_con = xb_mysql_connect();
  if (!mdl_con)
  {
    msg("FATAL: cannot create connection for MDL locks");
    exit(1);
  }
  const char *query =
    "SELECT NAME, SPACE FROM INFORMATION_SCHEMA.INNODB_SYS_TABLES WHERE NAME LIKE '%%/%%'";

  MYSQL_RES *mysql_result = xb_mysql_query(mdl_con, query, true, true);
  while (MYSQL_ROW row = mysql_fetch_row(mysql_result)) {
    int err;
    ulint id = (ulint)my_strtoll10(row[1], 0, &err);
    spaceid_to_tablename[id] = ut_get_name(0, row[0]);
  }
  mysql_free_result(mysql_result);

  xb_mysql_query(mdl_con, "BEGIN", false, true);
}

void
mdl_lock_table(ulint space_id)
{
  if (space_id == 0)
    return;

  std::string full_table_name = spaceid_to_tablename[space_id];

  DBUG_EXECUTE_IF("rename_during_mdl_lock_table",
    if (full_table_name == "`test`.`t1`")
      xb_mysql_query(mysql_connection, "RENAME TABLE test.t1 to test.t2", false, true);
  );

  std::ostringstream lock_query;
  lock_query << "SELECT 1 FROM " << full_table_name  << " LIMIT 0";
  msg("Locking MDL for %s", full_table_name.c_str());
  if (mysql_query(mdl_con, lock_query.str().c_str())) {
      msg("Warning : locking MDL failed for space id %zu, name %s", space_id, full_table_name.c_str());
  } else {
      MYSQL_RES *r = mysql_store_result(mdl_con);
      mysql_free_result(r);
  }
}

void
mdl_unlock_all()
{
  msg("Unlocking MDL for all tables");
  xb_mysql_query(mdl_con, "COMMIT", false, true);
  mysql_close(mdl_con);
  spaceid_to_tablename.clear();
}
