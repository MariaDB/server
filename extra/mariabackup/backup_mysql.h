#ifndef XTRABACKUP_BACKUP_MYSQL_H
#define XTRABACKUP_BACKUP_MYSQL_H

#include <mysql.h>
#include <string>
#include <unordered_set>
#include "datasink.h"

/* MariaDB version */
extern ulong mysql_server_version;

/* server capabilities */
extern bool have_changed_page_bitmaps;
extern bool have_lock_wait_timeout;
extern bool have_galera_enabled;
extern bool have_multi_threaded_slave;
extern bool have_gtid_slave;


/* History on server */
extern time_t history_start_time;
extern time_t history_end_time;
extern time_t history_lock_time;


extern bool sql_thread_started;
extern char *mysql_slave_position;
extern char *mysql_binlog_position;
extern char *buffer_pool_filename;

/** connection to mysql server */
extern MYSQL *mysql_connection;

void
capture_tool_command(int argc, char **argv);

bool
select_history();

void
backup_cleanup();

bool
get_mysql_vars(MYSQL *connection);

MYSQL *
xb_mysql_connect();

MYSQL_RES *
xb_mysql_query(MYSQL *connection, const char *query, bool use_result,
		bool die_on_error = true);

void
unlock_all(MYSQL *connection);

bool
write_current_binlog_file(ds_ctxt *datasink, MYSQL *connection);

bool
write_binlog_info(ds_ctxt *datasink, MYSQL *connection);

bool
write_xtrabackup_info(ds_ctxt *datasink,
                      MYSQL *connection, const char * filename, bool history,
                      bool stream);

bool
write_backup_config_file(ds_ctxt *datasink);

bool
lock_binlog_maybe(MYSQL *connection);

bool
lock_for_backup_stage_start(MYSQL *connection);

bool
lock_for_backup_stage_flush(MYSQL *connection);

bool
lock_for_backup_stage_block_ddl(MYSQL *connection);

bool
lock_for_backup_stage_commit(MYSQL *connection);

bool backup_lock(MYSQL *con, const char *table_name);
bool backup_unlock(MYSQL *con);

std::unordered_set<std::string> get_tables_in_use(MYSQL *con);

bool
wait_for_safe_slave(MYSQL *connection);

bool
write_galera_info(ds_ctxt *datasink, MYSQL *connection);

bool
write_slave_info(ds_ctxt *datasink, MYSQL *connection);

ulonglong get_current_lsn(MYSQL *connection);

#endif
