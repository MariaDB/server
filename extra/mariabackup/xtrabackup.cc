/******************************************************
MariaBackup: hot backup tool for InnoDB
(c) 2009-2017 Percona LLC and/or its affiliates
Originally Created 3/3/2009 Yasufumi Kinoshita
Written by Alexey Kopytov, Aleksandr Kuzminsky, Stewart Smith, Vadim Tkachenko,
Yasufumi Kinoshita, Ignacio Nin and Baron Schwartz.
(c) 2017, 2019, MariaDB Corporation.
Portions written by Marko Mäkelä.

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

//#define XTRABACKUP_TARGET_IS_PLUGIN

#include <my_config.h>
#include <unireg.h>
#include <mysql_version.h>
#include <my_base.h>
#include <my_getopt.h>
#include <mysql_com.h>
#include <my_default.h>
#include <mysqld.h>

#include <fcntl.h>
#include <string.h>

#ifdef __linux__
# include <sys/prctl.h>
#include <sys/resource.h>
#endif


#include <btr0sea.h>
#include <dict0priv.h>
#include <lock0lock.h>
#include <log0recv.h>
#include <log0crypt.h>
#include <row0mysql.h>
#include <row0quiesce.h>
#include <srv0start.h>
#include "trx0sys.h"
#include <buf0dblwr.h>
#include "ha_innodb.h"

#include <list>
#include <sstream>
#include <set>
#include <fstream>
#include <mysql.h>

#define G_PTR uchar*

#include "common.h"
#include "datasink.h"

#include "xb_regex.h"
#include "fil_cur.h"
#include "write_filt.h"
#include "xtrabackup.h"
#include "ds_buffer.h"
#include "ds_tmpfile.h"
#include "xbstream.h"
#include "changed_page_bitmap.h"
#include "read_filt.h"
#include "backup_wsrep.h"
#include "innobackupex.h"
#include "backup_mysql.h"
#include "backup_copy.h"
#include "backup_mysql.h"
#include "xb0xb.h"
#include "encryption_plugin.h"
#include <sql_plugin.h>
#include <srv0srv.h>
#include <crc_glue.h>
#include <log.h>
#include <derror.h>
#include "backup_debug.h"

#define MB_CORRUPTED_PAGES_FILE "innodb_corrupted_pages"

int sys_var_init();

/* === xtrabackup specific options === */
char xtrabackup_real_target_dir[FN_REFLEN] = "./xtrabackup_backupfiles/";
char *xtrabackup_target_dir= xtrabackup_real_target_dir;
static my_bool xtrabackup_version;
static my_bool verbose;
my_bool xtrabackup_backup;
my_bool xtrabackup_prepare;
my_bool xtrabackup_copy_back;
my_bool xtrabackup_move_back;
my_bool xtrabackup_decrypt_decompress;
my_bool xtrabackup_print_param;

my_bool xtrabackup_export;

my_bool xtrabackup_rollback_xa;

longlong xtrabackup_use_memory;

uint opt_protocol;
long xtrabackup_throttle; /* 0:unlimited */
static lint io_ticket;
static os_event_t wait_throttle;
static os_event_t log_copying_stop;

char *xtrabackup_incremental;
lsn_t incremental_lsn;
lsn_t incremental_to_lsn;
lsn_t incremental_last_lsn;
xb_page_bitmap *changed_page_bitmap;

char *xtrabackup_incremental_basedir; /* for --backup */
char *xtrabackup_extra_lsndir; /* for --backup with --extra-lsndir */
char *xtrabackup_incremental_dir; /* for --prepare */

char xtrabackup_real_incremental_basedir[FN_REFLEN];
char xtrabackup_real_extra_lsndir[FN_REFLEN];
char xtrabackup_real_incremental_dir[FN_REFLEN];


char *xtrabackup_tmpdir;

char *xtrabackup_tables;
char *xtrabackup_tables_file;
char *xtrabackup_tables_exclude;
char *xb_rocksdb_datadir;
my_bool xb_backup_rocksdb = 1;

typedef std::list<regex_t> regex_list_t;
static regex_list_t regex_include_list;
static regex_list_t regex_exclude_list;

static hash_table_t* tables_include_hash = NULL;
static hash_table_t* tables_exclude_hash = NULL;

char *xtrabackup_databases = NULL;
char *xtrabackup_databases_file = NULL;
char *xtrabackup_databases_exclude = NULL;
static hash_table_t* databases_include_hash = NULL;
static hash_table_t* databases_exclude_hash = NULL;

static hash_table_t* inc_dir_tables_hash;

struct xb_filter_entry_struct{
	char*		name;
	ibool		has_tables;
	hash_node_t	name_hash;
};
typedef struct xb_filter_entry_struct	xb_filter_entry_t;

lsn_t checkpoint_lsn_start;
lsn_t checkpoint_no_start;
static lsn_t log_copy_scanned_lsn;
static bool log_copying_running;
static bool io_watching_thread_running;

int xtrabackup_parallel;

char *xtrabackup_stream_str = NULL;
xb_stream_fmt_t xtrabackup_stream_fmt = XB_STREAM_FMT_NONE;
ibool xtrabackup_stream = FALSE;

const char *xtrabackup_compress_alg = NULL;
uint xtrabackup_compress = FALSE;
uint xtrabackup_compress_threads;
ulonglong xtrabackup_compress_chunk_size = 0;

/* sleep interval beetween log copy iterations in log copying thread
in milliseconds (default is 1 second) */
ulint xtrabackup_log_copy_interval = 1000;
static ulong max_buf_pool_modified_pct;

/* Ignored option (--log) for MySQL option compatibility */
static char*	log_ignored_opt;


extern my_bool opt_use_ssl;
my_bool opt_ssl_verify_server_cert;
my_bool opt_extended_validation;
my_bool opt_encrypted_backup;

/* === metadata of backup === */
#define XTRABACKUP_METADATA_FILENAME "xtrabackup_checkpoints"
char metadata_type[30] = ""; /*[full-backuped|log-applied|incremental]*/
static lsn_t metadata_from_lsn;
lsn_t metadata_to_lsn;
static lsn_t metadata_last_lsn;

static ds_file_t*	dst_log_file;

static char mysql_data_home_buff[2];

const char *defaults_group = "mysqld";

/* === static parameters in ha_innodb.cc */

#define HA_INNOBASE_ROWS_IN_TABLE 10000 /* to get optimization right */
#define HA_INNOBASE_RANGE_COUNT	  100

/* The default values for the following, type long or longlong, start-up
parameters are declared in mysqld.cc: */

long innobase_buffer_pool_awe_mem_mb = 0;
long innobase_file_io_threads = 4;
long innobase_read_io_threads = 4;
long innobase_write_io_threads = 4;
long innobase_log_buffer_size = 1024*1024L;
long innobase_open_files = 300L;

longlong innobase_page_size = (1LL << 14); /* 16KB */
char*	innobase_buffer_pool_filename = NULL;

/* The default values for the following char* start-up parameters
are determined in innobase_init below: */

static char*	innobase_ignored_opt;
char*	innobase_data_home_dir;
char*	innobase_data_file_path;
/* The following has a misleading name: starting from 4.0.5, this also
affects Windows: */
char*	innobase_unix_file_flush_method;

my_bool innobase_use_doublewrite;
my_bool	innobase_file_per_table;
my_bool innobase_locks_unsafe_for_binlog;
my_bool innobase_rollback_on_timeout;
my_bool innobase_create_status_file;

/* The following counter is used to convey information to InnoDB
about server activity: in selects it is not sensible to call
srv_active_wake_master_thread after each fetch or search, we only do
it every INNOBASE_WAKE_INTERVAL'th step. */

#define INNOBASE_WAKE_INTERVAL	32
ulong	innobase_active_counter	= 0;


my_bool xtrabackup_incremental_force_scan = FALSE;

/*
 * Ignore corrupt pages (disabled by default; used
 * by "innobackupex" as a command line argument).
 */
ulong xtrabackup_innodb_force_recovery = 0;

/* The flushed lsn which is read from data files */
lsn_t	flushed_lsn= 0;

ulong xb_open_files_limit= 0;
char *xb_plugin_dir;
char *xb_plugin_load;
my_bool xb_close_files;

/* Datasinks */
ds_ctxt_t       *ds_data     = NULL;
ds_ctxt_t       *ds_meta     = NULL;
ds_ctxt_t       *ds_redo     = NULL;

static bool	innobackupex_mode = false;

/* String buffer used by --print-param to accumulate server options as they are
parsed from the defaults file */
static std::ostringstream print_param_str;

/* Set of specified parameters */
std::set<std::string> param_set;

static ulonglong global_max_value;

extern "C" sig_handler handle_fatal_signal(int sig);
extern LOGGER logger;

my_bool opt_galera_info = FALSE;
my_bool opt_slave_info = FALSE;
my_bool opt_no_lock = FALSE;
my_bool opt_safe_slave_backup = FALSE;
my_bool opt_rsync = FALSE;
my_bool opt_force_non_empty_dirs = FALSE;
my_bool opt_noversioncheck = FALSE;
my_bool opt_no_backup_locks = FALSE;
my_bool opt_decompress = FALSE;
my_bool opt_remove_original;
my_bool opt_log_innodb_page_corruption;

my_bool opt_lock_ddl_per_table = FALSE;
static my_bool opt_check_privileges;

static const char *binlog_info_values[] = {"off", "lockless", "on", "auto",
					   NullS};
static TYPELIB binlog_info_typelib = {array_elements(binlog_info_values)-1, "",
				      binlog_info_values, NULL};
ulong opt_binlog_info;

char *opt_incremental_history_name;
char *opt_incremental_history_uuid;

char *opt_user;
char *opt_password;
char *opt_host;
char *opt_defaults_group;
char *opt_socket;
uint opt_port;
char *opt_log_bin;

const char *query_type_names[] = { "ALL", "UPDATE", "SELECT", NullS};

TYPELIB query_type_typelib= {array_elements(query_type_names) - 1, "",
	query_type_names, NULL};

ulong opt_lock_wait_query_type;
ulong opt_kill_long_query_type;

uint opt_kill_long_queries_timeout = 0;
uint opt_lock_wait_timeout = 0;
uint opt_lock_wait_threshold = 0;
uint opt_debug_sleep_before_unlock = 0;
uint opt_safe_slave_backup_timeout = 0;

const char *opt_history = NULL;


char mariabackup_exe[FN_REFLEN];
char orig_argv1[FN_REFLEN];

pthread_mutex_t backup_mutex;
pthread_cond_t  scanned_lsn_cond;

typedef std::map<space_id_t,std::string> space_id_to_name_t;

struct ddl_tracker_t {
	/** Tablspaces with their ID and name, as they were copied to backup.*/
	space_id_to_name_t tables_in_backup;
	/** Tablespaces for that optimized DDL without redo log was found.*/
	std::set<space_id_t> optimized_ddl;
	/** Drop operations found in redo log. */
	std::set<space_id_t> drops;
	/* For DDL operation found in redo log,  */
	space_id_to_name_t id_to_name;
};

static ddl_tracker_t ddl_tracker;

// Convert non-null terminated filename to space name
std::string filename_to_spacename(const byte *filename, size_t len);

CorruptedPages::CorruptedPages() { ut_a(!pthread_mutex_init(&m_mutex, NULL)); }

CorruptedPages::~CorruptedPages() { ut_a(!pthread_mutex_destroy(&m_mutex)); }

void CorruptedPages::add_page_no_lock(const char *space_name, ulint space_id,
                                      ulint page_no, bool convert_space_name)
{
  space_info_t  &space_info = m_spaces[space_id];
  if (space_info.space_name.empty())
    space_info.space_name=
        convert_space_name
            ? filename_to_spacename(reinterpret_cast<const byte *>(space_name),
                                    strlen(space_name))
            : space_name;
  (void)space_info.pages.insert(page_no);
}

void CorruptedPages::add_page(const char *file_name, ulint space_id,
                              ulint page_no)
{
  ut_a(!pthread_mutex_lock(&m_mutex));
  add_page_no_lock(file_name, space_id, page_no, true);
  ut_a(!pthread_mutex_unlock(&m_mutex));
}

bool CorruptedPages::contains(ulint space_id, ulint page_no) const
{
  bool result = false;
  ut_a(!pthread_mutex_lock(&m_mutex));
  container_t::const_iterator space_it= m_spaces.find(space_id);
  if (space_it != m_spaces.end())
    result = space_it->second.pages.count(page_no);
  ut_a(!pthread_mutex_unlock(&m_mutex));
  return result;
}

void CorruptedPages::drop_space(ulint space_id)
{
  ut_a(!pthread_mutex_lock(&m_mutex));
  m_spaces.erase(space_id);
  ut_a(!pthread_mutex_unlock(&m_mutex));
}

void CorruptedPages::rename_space(ulint space_id, const std::string &new_name)
{
  ut_a(!pthread_mutex_lock(&m_mutex));
  container_t::iterator space_it = m_spaces.find(space_id);
  if (space_it != m_spaces.end())
    space_it->second.space_name = new_name;
  ut_a(!pthread_mutex_unlock(&m_mutex));
}

bool CorruptedPages::print_to_file(const char *filename) const
{
  std::ostringstream out;
  ut_a(!pthread_mutex_lock(&m_mutex));
  if (!m_spaces.size())
  {
    ut_a(!pthread_mutex_unlock(&m_mutex));
    return true;
  }
  for (container_t::const_iterator space_it=
           m_spaces.begin();
       space_it != m_spaces.end(); ++space_it)
  {
    out << space_it->second.space_name << " " << space_it->first << "\n";
    bool first_page_no= true;
    for (std::set<ulint>::const_iterator page_it=
             space_it->second.pages.begin();
         page_it != space_it->second.pages.end(); ++page_it)
      if (first_page_no)
      {
        out << *page_it;
        first_page_no= false;
      }
      else
        out << " " << *page_it;
    out << "\n";
  }
  ut_a(!pthread_mutex_unlock(&m_mutex));
  if (xtrabackup_backup)
    return backup_file_print_buf(filename, out.str().c_str(),
                                 out.str().size());
  std::ofstream outfile;
  outfile.open(filename);
  if (!outfile.is_open())
    die("Can't open %s, error number: %d, error message: %s", filename, errno,
        strerror(errno));
  outfile << out.str();
  return true;
}

void CorruptedPages::read_from_file(const char *file_name)
{
  MY_STAT mystat;
  if (!my_stat(file_name, &mystat, MYF(0)))
    return;
  std::ifstream infile;
  infile.open(file_name);
  if (!infile.is_open())
    die("Can't open %s, error number: %d, error message: %s", file_name, errno,
        strerror(errno));
  std::string line;
  std::string space_name;
  ulint space_id;
  ulint line_number= 0;
  while (std::getline(infile, line))
  {
    ++line_number;
    std::istringstream iss(line);
    if (line_number & 1) {
      if (!(iss >> space_name))
        die("Can't parse space name from corrupted pages file at "
            "line " ULINTPF,
            line_number);
      if (!(iss >> space_id))
        die("Can't parse space id from corrupted pages file at line " ULINTPF,
            line_number);
    }
    else
    {
      ulint page_no;
      while ((iss >> page_no))
        add_page_no_lock(space_name.c_str(), space_id, page_no, false);
      if (!iss.eof())
        die("Corrupted pages file parse error on line number " ULINTPF,
            line_number);
    }
  }
}

bool CorruptedPages::empty() const
{
  ut_a(!pthread_mutex_lock(&m_mutex));
  bool result= !m_spaces.size();
  ut_a(!pthread_mutex_unlock(&m_mutex));
  return result;
}

static void xb_load_single_table_tablespace(const std::string &space_name,
                                            bool set_size);
static void xb_data_files_close();

void CorruptedPages::zero_out_free_pages()
{
  container_t non_free_pages;
  byte* buf= static_cast<byte*>(ut_malloc_nokey(2 * UNIV_PAGE_SIZE));
  byte* zero_page = static_cast<byte*>(ut_align(buf, UNIV_PAGE_SIZE));
  memset(zero_page, 0, UNIV_PAGE_SIZE);

  ut_a(!pthread_mutex_lock(&m_mutex));
  for (container_t::const_iterator space_it= m_spaces.begin();
       space_it != m_spaces.end(); ++space_it)
  {
    ulint space_id = space_it->first;
    const std::string &space_name = space_it->second.space_name;
    // There is no need to close tablespaces explixitly as they will be closed
    // in innodb_shutdown().
    xb_load_single_table_tablespace(space_name, false);
    mutex_enter(&fil_system->mutex);
    fil_space_t *space = fil_space_get_by_name(space_name.c_str());
    mutex_exit(&fil_system->mutex);
    if (!space)
      die("Can't find space object for space name %s to check corrupted page",
          space_name.c_str());
    for (std::set<ulint>::const_iterator page_it=
             space_it->second.pages.begin();
         page_it != space_it->second.pages.end(); ++page_it)
    {
      bool is_free= fseg_page_is_free(space, *page_it);
      if (!is_free) {
        space_info_t &space_info = non_free_pages[space_id];
        space_info.pages.insert(*page_it);
        if (space_info.space_name.empty())
          space_info.space_name = space_name;
        msg("Error: corrupted page " ULINTPF
            " of tablespace %s can not be fixed",
            *page_it, space_name.c_str());
      }
      else
      {
        const page_id_t page_id(space->id, *page_it);
        dberr_t err= fil_io(IORequestWrite, true, page_id, univ_page_size, 0,
                            univ_page_size.physical(), zero_page, NULL);
        if (err != DB_SUCCESS)
          die("Can't zero out corrupted page " ULINTPF " of tablespace %s",
              *page_it, space_name.c_str());
        msg("Corrupted page " ULINTPF
            " of tablespace %s was successfuly fixed.",
            *page_it, space_name.c_str());
      }
    }
  }
  m_spaces.swap(non_free_pages);
  ut_a(!pthread_mutex_unlock(&m_mutex));
  ut_free(buf);
}

/* Simple datasink creation tracking...add datasinks in the reverse order you
want them destroyed. */
#define XTRABACKUP_MAX_DATASINKS	10
static	ds_ctxt_t	*datasinks[XTRABACKUP_MAX_DATASINKS];
static	uint		actual_datasinks = 0;
static inline
void
xtrabackup_add_datasink(ds_ctxt_t *ds)
{
	xb_ad(actual_datasinks < XTRABACKUP_MAX_DATASINKS);
	datasinks[actual_datasinks] = ds; actual_datasinks++;
}

typedef void (*process_single_tablespace_func_t)(const char *dirname,
                                                 const char *filname,
                                                 bool is_remote,
                                                 bool set_size);
static dberr_t enumerate_ibd_files(process_single_tablespace_func_t callback);

/* ======== Datafiles iterator ======== */
struct datafiles_iter_t {
	fil_system_t	*system;
	fil_space_t	*space;
	fil_node_t	*node;
	ibool		started;
	pthread_mutex_t	mutex;
};

/* ======== Datafiles iterator ======== */
static
datafiles_iter_t *
datafiles_iter_new(fil_system_t *f_system)
{
	datafiles_iter_t *it;

	it = static_cast<datafiles_iter_t *>(malloc(sizeof(datafiles_iter_t)));
	pthread_mutex_init(&it->mutex, NULL);

	it->system = f_system;
	it->space = NULL;
	it->node = NULL;
	it->started = FALSE;

	return it;
}

static
fil_node_t *
datafiles_iter_next(datafiles_iter_t *it)
{
	fil_node_t *new_node;

	pthread_mutex_lock(&it->mutex);

	if (it->node == NULL) {
		if (it->started)
			goto end;
		it->started = TRUE;
	} else {
		it->node = UT_LIST_GET_NEXT(chain, it->node);
		if (it->node != NULL)
			goto end;
	}

	it->space = (it->space == NULL) ?
		UT_LIST_GET_FIRST(it->system->space_list) :
		UT_LIST_GET_NEXT(space_list, it->space);

	while (it->space != NULL &&
	       (it->space->purpose != FIL_TYPE_TABLESPACE ||
		UT_LIST_GET_LEN(it->space->chain) == 0))
		it->space = UT_LIST_GET_NEXT(space_list, it->space);
	if (it->space == NULL)
		goto end;

	it->node = UT_LIST_GET_FIRST(it->space->chain);

end:
	new_node = it->node;
	pthread_mutex_unlock(&it->mutex);

	return new_node;
}

static
void
datafiles_iter_free(datafiles_iter_t *it)
{
	pthread_mutex_destroy(&it->mutex);
	free(it);
}

#ifndef DBUG_OFF
struct dbug_thread_param_t
{
	MYSQL *con;
	const char *query;
	int expect_err;
	int expect_errno;
	os_event_t done_event;
};


/* Thread procedure used in dbug_start_query_thread. */
extern "C"
os_thread_ret_t
DECLARE_THREAD(dbug_execute_in_new_connection)(void *arg)
{
	mysql_thread_init();
	dbug_thread_param_t *par= (dbug_thread_param_t *)arg;
	int err = mysql_query(par->con, par->query);
	int err_no = mysql_errno(par->con);
	DBUG_ASSERT(par->expect_err == err);
	if (err && par->expect_errno)
		DBUG_ASSERT(err_no == par->expect_errno);
	mysql_close(par->con);
	mysql_thread_end();
	os_event_t done = par->done_event;
	delete par;
	os_event_set(done);
	os_thread_exit();
	return os_thread_ret_t(0);
}

/*
Execute query from a new connection, in own thread.

@param query - query to be executed
@param wait_state - if not NULL, wait until query from new connection
	reaches this state (value of column State in I_S.PROCESSLIST)
@param expected_err - if 0, query is supposed to finish successfully,
	otherwise query should return error.
@param expected_errno - if not 0, and query finished with error,
	expected mysql_errno()
*/
static os_event_t dbug_start_query_thread(
	const char *query,
	const char *wait_state,
	int expected_err,
	int expected_errno)

{
	dbug_thread_param_t *par = new dbug_thread_param_t;
	par->query = query;
	par->expect_err = expected_err;
	par->expect_errno = expected_errno;
	par->done_event = os_event_create(0);
	par->con =  xb_mysql_connect();
	os_thread_create(dbug_execute_in_new_connection, par, 0);

	if (!wait_state)
		return par->done_event;

	char q[256];
	snprintf(q, sizeof(q),
		"SELECT 1 FROM INFORMATION_SCHEMA.PROCESSLIST where ID=%lu"
		" AND Command='Query' AND State='%s'",
		mysql_thread_id(par->con), wait_state);
	for (;;) {
		MYSQL_RES *result = xb_mysql_query(mysql_connection,q, true, true);
		bool exists = mysql_fetch_row(result) != NULL;
		mysql_free_result(result);
		if (exists) {
			goto end;
		}
		msg("Waiting for query '%s' on connection %lu to "
			" reach state '%s'", query, mysql_thread_id(par->con),
			wait_state);
		my_sleep(1000);
	}
end:
	msg("query '%s' on connection %lu reached state '%s'", query,
	mysql_thread_id(par->con), wait_state);
	return par->done_event;
}

os_event_t dbug_alter_thread_done;
#endif

void mdl_lock_all()
{
	mdl_lock_init();
	datafiles_iter_t *it = datafiles_iter_new(fil_system);
	if (!it)
		return;

	while (fil_node_t *node = datafiles_iter_next(it)){
		if (fil_is_user_tablespace_id(node->space->id)
			&& check_if_skip_table(node->space->name))
			continue;

		mdl_lock_table(node->space->id);
	}
	datafiles_iter_free(it);
}


// Convert non-null terminated filename to space name
std::string filename_to_spacename(const byte *filename, size_t len)
{
	// null- terminate filename
	char *f = (char *)malloc(len + 1);
	ut_a(f);
	memcpy(f, filename, len);
	f[len] = 0;
	for (size_t i = 0; i < len; i++)
		if (f[i] == '\\')
			f[i] = '/';
	char *p = strrchr(f, '.');
	ut_a(p);
	*p = 0;
	char *table = strrchr(f, '/');
	ut_a(table);
	*table = 0;
	char *db = strrchr(f, '/');
	ut_a(db);
	*table = '/';
	std::string s(db+1);
	free(f);
	return s;
}

/** Report an operation to create, delete, or rename a file during backup.
@param[in]	space_id	tablespace identifier
@param[in]	flags		tablespace flags (NULL if not create)
@param[in]	name		file name (not NUL-terminated)
@param[in]	len		length of name, in bytes
@param[in]	new_name	new file name (NULL if not rename)
@param[in]	new_len		length of new_name, in bytes (0 if NULL) */
static void backup_file_op(ulint space_id, const byte* flags,
	const byte* name, ulint len,
	const byte* new_name, ulint new_len)
{

	ut_ad(!flags || !new_name);
	ut_ad(name);
	ut_ad(len);
	ut_ad(!new_name == !new_len);
	pthread_mutex_lock(&backup_mutex);

	if (flags) {
		ddl_tracker.id_to_name[space_id] = filename_to_spacename(name, len);
		msg("DDL tracking :  create %zu \"%.*s\": %x",
			space_id, int(len), name, mach_read_from_4(flags));
	}
	else if (new_name) {
		ddl_tracker.id_to_name[space_id] = filename_to_spacename(new_name, new_len);
		msg("DDL tracking : rename %zu \"%.*s\",\"%.*s\"",
			space_id, int(len), name, int(new_len), new_name);
	} else {
		ddl_tracker.drops.insert(space_id);
		msg("DDL tracking : delete %zu \"%.*s\"", space_id, int(len), name);
	}
	pthread_mutex_unlock(&backup_mutex);
}


/*
 This callback is called if DDL operation is detected,
 at the end of backup

 Normally, DDL operations are blocked due to FTWRL,
 but in rare cases of --no-lock, they are not.

 We will abort backup in this case.
*/
static void backup_file_op_fail(ulint space_id, const byte* flags,
	const byte* name, ulint len,
	const byte* new_name, ulint new_len)
{
	ut_a(opt_no_lock);
	bool fail;
	if (flags) {
		msg("DDL tracking :  create %zu \"%.*s\": %x",
			space_id, int(len), name, mach_read_from_4(flags));
		std::string  spacename = filename_to_spacename(name, len);
		fail = !check_if_skip_table(spacename.c_str());
	}
	else if (new_name) {
		msg("DDL tracking : rename %zu \"%.*s\",\"%.*s\"",
			space_id, int(len), name, int(new_len), new_name);
		std::string  spacename = filename_to_spacename(name, len);
		std::string  new_spacename = filename_to_spacename(new_name, new_len);
		fail = !check_if_skip_table(spacename.c_str()) || !check_if_skip_table(new_spacename.c_str());
	}
	else {
		std::string  spacename = filename_to_spacename(name, len);
		fail = !check_if_skip_table(spacename.c_str());
		msg("DDL tracking : delete %zu \"%.*s\"", space_id, int(len), name);
	}
	if (fail) {
		die("DDL operation detected in the late phase of backup."
			"Backup is inconsistent. Remove --no-lock option to fix.");
	}
}


/** Callback whenever MLOG_INDEX_LOAD happens.
@param[in]	space_id	space id to check */
static void backup_optimized_ddl_op(ulint space_id)
{
	pthread_mutex_lock(&backup_mutex);
	ddl_tracker.optimized_ddl.insert(space_id);
	pthread_mutex_unlock(&backup_mutex);
}

/*
  Optimized DDL callback at the end of backup that
  run with --no-lock. Usually aborts the backup.
*/
static void backup_optimized_ddl_op_fail(ulint space_id) {
	ut_a(opt_no_lock);
	msg("DDL tracking : optimized DDL on space %zu", space_id);
	if (ddl_tracker.tables_in_backup.find(space_id) != ddl_tracker.tables_in_backup.end()) {
		msg("ERROR : Optimized DDL operation detected in the late phase of backup."
			"Backup is inconsistent. Remove --no-lock option to fix.");
		exit(EXIT_FAILURE);
	}
}


/** Callback whenever MLOG_TRUNCATE happens. */
static void backup_truncate_fail()
{
	msg("mariabackup: Incompatible TRUNCATE operation detected.%s",
	    opt_lock_ddl_per_table
	    ? ""
	    : " Use --lock-ddl-per-table to lock all tables before backup.");
}


/*
  Retrieve default data directory, to be used with --copy-back.

  On Windows, default datadir is ..\data, relative to the
  directory where mariabackup.exe is located(usually "bin")

  Elsewhere, the compiled-in constant MYSQL_DATADIR is used.
*/
static char *get_default_datadir() {
	static char ddir[] = MYSQL_DATADIR;
#ifdef _WIN32
	static char buf[MAX_PATH];
	DWORD size = (DWORD)sizeof(buf) - 1;
	if (GetModuleFileName(NULL, buf, size) <= size)
	{
		char *p;
		if ((p = strrchr(buf, '\\')))
		{
			*p = 0;
			if ((p = strrchr(buf, '\\')))
			{
				strncpy(p + 1, "data", buf + MAX_PATH - p);
				return buf;
			}
		}
	}
#endif
	return ddir;
}


/* ======== Date copying thread context ======== */

typedef struct {
	datafiles_iter_t 	*it;
	uint			num;
	uint			*count;
	pthread_mutex_t*	count_mutex;
	os_thread_id_t		id;
	CorruptedPages *corrupted_pages;
} data_thread_ctxt_t;

/* ======== for option and variables ======== */
#include <../../client/client_priv.h>

enum options_xtrabackup
{
  OPT_XTRA_TARGET_DIR= 1000, /* make sure it is larger
                                than OPT_MAX_CLIENT_OPTION */
  OPT_XTRA_BACKUP,
  OPT_XTRA_PREPARE,
  OPT_XTRA_EXPORT,
  OPT_XTRA_ROLLBACK_XA,
  OPT_XTRA_PRINT_PARAM,
  OPT_XTRA_USE_MEMORY,
  OPT_XTRA_THROTTLE,
  OPT_XTRA_LOG_COPY_INTERVAL,
  OPT_XTRA_INCREMENTAL,
  OPT_XTRA_INCREMENTAL_BASEDIR,
  OPT_XTRA_EXTRA_LSNDIR,
  OPT_XTRA_INCREMENTAL_DIR,
  OPT_XTRA_TABLES,
  OPT_XTRA_TABLES_FILE,
  OPT_XTRA_DATABASES,
  OPT_XTRA_DATABASES_FILE,
  OPT_XTRA_PARALLEL,
  OPT_XTRA_EXTENDED_VALIDATION,
  OPT_XTRA_ENCRYPTED_BACKUP,
  OPT_XTRA_STREAM,
  OPT_XTRA_COMPRESS,
  OPT_XTRA_COMPRESS_THREADS,
  OPT_XTRA_COMPRESS_CHUNK_SIZE,
  OPT_LOG,
  OPT_INNODB,
  OPT_INNODB_DATA_FILE_PATH,
  OPT_INNODB_DATA_HOME_DIR,
  OPT_INNODB_ADAPTIVE_HASH_INDEX,
  OPT_INNODB_DOUBLEWRITE,
  OPT_INNODB_FILE_PER_TABLE,
  OPT_INNODB_FLUSH_LOG_AT_TRX_COMMIT,
  OPT_INNODB_FLUSH_METHOD,
  OPT_INNODB_LOCKS_UNSAFE_FOR_BINLOG,
  OPT_INNODB_LOG_GROUP_HOME_DIR,
  OPT_INNODB_MAX_DIRTY_PAGES_PCT,
  OPT_INNODB_MAX_PURGE_LAG,
  OPT_INNODB_ROLLBACK_ON_TIMEOUT,
  OPT_INNODB_STATUS_FILE,
  OPT_INNODB_AUTOEXTEND_INCREMENT,
  OPT_INNODB_BUFFER_POOL_SIZE,
  OPT_INNODB_COMMIT_CONCURRENCY,
  OPT_INNODB_CONCURRENCY_TICKETS,
  OPT_INNODB_FILE_IO_THREADS,
  OPT_INNODB_IO_CAPACITY,
  OPT_INNODB_READ_IO_THREADS,
  OPT_INNODB_WRITE_IO_THREADS,
  OPT_INNODB_USE_NATIVE_AIO,
  OPT_INNODB_PAGE_SIZE,
  OPT_INNODB_BUFFER_POOL_FILENAME,
  OPT_INNODB_LOCK_WAIT_TIMEOUT,
  OPT_INNODB_LOG_BUFFER_SIZE,
  OPT_INNODB_LOG_FILE_SIZE,
  OPT_INNODB_LOG_FILES_IN_GROUP,
  OPT_INNODB_OPEN_FILES,
  OPT_XTRA_DEBUG_SYNC,
  OPT_INNODB_CHECKSUM_ALGORITHM,
  OPT_INNODB_UNDO_DIRECTORY,
  OPT_INNODB_UNDO_TABLESPACES,
  OPT_INNODB_LOG_CHECKSUMS,
  OPT_XTRA_INCREMENTAL_FORCE_SCAN,
  OPT_DEFAULTS_GROUP,
  OPT_CLOSE_FILES,
  OPT_CORE_FILE,

  OPT_COPY_BACK,
  OPT_MOVE_BACK,
  OPT_GALERA_INFO,
  OPT_SLAVE_INFO,
  OPT_NO_LOCK,
  OPT_SAFE_SLAVE_BACKUP,
  OPT_RSYNC,
  OPT_FORCE_NON_EMPTY_DIRS,
  OPT_NO_VERSION_CHECK,
  OPT_NO_BACKUP_LOCKS,
  OPT_DECOMPRESS,
  OPT_INCREMENTAL_HISTORY_NAME,
  OPT_INCREMENTAL_HISTORY_UUID,
  OPT_REMOVE_ORIGINAL,
  OPT_LOCK_WAIT_QUERY_TYPE,
  OPT_KILL_LONG_QUERY_TYPE,
  OPT_HISTORY,
  OPT_KILL_LONG_QUERIES_TIMEOUT,
  OPT_LOCK_WAIT_TIMEOUT,
  OPT_LOCK_WAIT_THRESHOLD,
  OPT_DEBUG_SLEEP_BEFORE_UNLOCK,
  OPT_SAFE_SLAVE_BACKUP_TIMEOUT,
  OPT_BINLOG_INFO,
  OPT_XB_SECURE_AUTH,

  OPT_XTRA_TABLES_EXCLUDE,
  OPT_XTRA_DATABASES_EXCLUDE,
  OPT_PROTOCOL,
  OPT_INNODB_COMPRESSION_LEVEL,
  OPT_LOCK_DDL_PER_TABLE,
  OPT_ROCKSDB_DATADIR,
  OPT_BACKUP_ROCKSDB,
  OPT_XTRA_CHECK_PRIVILEGES,
  OPT_XB_IGNORE_INNODB_PAGE_CORRUPTION,
  OPT_INNODB_FORCE_RECOVERY
};

struct my_option xb_client_options[]= {
    {"verbose", 'V', "display verbose output", (G_PTR *) &verbose,
     (G_PTR *) &verbose, 0, GET_BOOL, NO_ARG, FALSE, 0, 0, 0, 0, 0},
    {"version", 'v', "print version information",
     (G_PTR *) &xtrabackup_version, (G_PTR *) &xtrabackup_version, 0, GET_BOOL,
     NO_ARG, 0, 0, 0, 0, 0, 0},
    {"target-dir", OPT_XTRA_TARGET_DIR, "destination directory",
     (G_PTR *) &xtrabackup_target_dir, (G_PTR *) &xtrabackup_target_dir, 0,
     GET_STR, REQUIRED_ARG, 0, 0, 0, 0, 0, 0},
    {"backup", OPT_XTRA_BACKUP, "take backup to target-dir",
     (G_PTR *) &xtrabackup_backup, (G_PTR *) &xtrabackup_backup, 0, GET_BOOL,
     NO_ARG, 0, 0, 0, 0, 0, 0},
    {"prepare", OPT_XTRA_PREPARE,
     "prepare a backup for starting mysql server on the backup.",
     (G_PTR *) &xtrabackup_prepare, (G_PTR *) &xtrabackup_prepare, 0, GET_BOOL,
     NO_ARG, 0, 0, 0, 0, 0, 0},
    {"export", OPT_XTRA_EXPORT,
     "create files to import to another database when prepare.",
     (G_PTR *) &xtrabackup_export, (G_PTR *) &xtrabackup_export, 0, GET_BOOL,
     NO_ARG, 0, 0, 0, 0, 0, 0},
    {"rollback-xa", OPT_XTRA_ROLLBACK_XA,
     "Rollback prepared XA's on --prepare. "
     "After preparing target directory with this option "
     "it can no longer be a base for incremental backup.",
     (G_PTR *) &xtrabackup_rollback_xa, (G_PTR *) &xtrabackup_rollback_xa, 0,
     GET_BOOL, NO_ARG, 0, 0, 0, 0, 0, 0},
    {"print-param", OPT_XTRA_PRINT_PARAM,
     "print parameter of mysqld needed for copyback.",
     (G_PTR *) &xtrabackup_print_param, (G_PTR *) &xtrabackup_print_param, 0,
     GET_BOOL, NO_ARG, 0, 0, 0, 0, 0, 0},
    {"use-memory", OPT_XTRA_USE_MEMORY,
     "The value is used instead of buffer_pool_size",
     (G_PTR *) &xtrabackup_use_memory, (G_PTR *) &xtrabackup_use_memory, 0,
     GET_LL, REQUIRED_ARG, 100 * 1024 * 1024L, 1024 * 1024L, LONGLONG_MAX, 0,
     1024 * 1024L, 0},
    {"throttle", OPT_XTRA_THROTTLE,
     "limit count of IO operations (pairs of read&write) per second to IOS "
     "values (for '--backup')",
     (G_PTR *) &xtrabackup_throttle, (G_PTR *) &xtrabackup_throttle, 0,
     GET_LONG, REQUIRED_ARG, 0, 0, LONG_MAX, 0, 1, 0},
    {"log", OPT_LOG, "Ignored option for MySQL option compatibility",
     (G_PTR *) &log_ignored_opt, (G_PTR *) &log_ignored_opt, 0, GET_STR,
     OPT_ARG, 0, 0, 0, 0, 0, 0},
    {"log-copy-interval", OPT_XTRA_LOG_COPY_INTERVAL,
     "time interval between checks done by log copying thread in milliseconds "
     "(default is 1 second).",
     (G_PTR *) &xtrabackup_log_copy_interval,
     (G_PTR *) &xtrabackup_log_copy_interval, 0, GET_LONG, REQUIRED_ARG, 1000,
     0, LONG_MAX, 0, 1, 0},
    {"extra-lsndir", OPT_XTRA_EXTRA_LSNDIR,
     "(for --backup): save an extra copy of the xtrabackup_checkpoints file "
     "in this directory.",
     (G_PTR *) &xtrabackup_extra_lsndir, (G_PTR *) &xtrabackup_extra_lsndir, 0,
     GET_STR, REQUIRED_ARG, 0, 0, 0, 0, 0, 0},
    {"incremental-lsn", OPT_XTRA_INCREMENTAL,
     "(for --backup): copy only .ibd pages newer than specified LSN "
     "'high:low'. ##ATTENTION##: If a wrong LSN value is specified, it is "
     "impossible to diagnose this, causing the backup to be unusable. Be "
     "careful!",
     (G_PTR *) &xtrabackup_incremental, (G_PTR *) &xtrabackup_incremental, 0,
     GET_STR, REQUIRED_ARG, 0, 0, 0, 0, 0, 0},
    {"incremental-basedir", OPT_XTRA_INCREMENTAL_BASEDIR,
     "(for --backup): copy only .ibd pages newer than backup at specified "
     "directory.",
     (G_PTR *) &xtrabackup_incremental_basedir,
     (G_PTR *) &xtrabackup_incremental_basedir, 0, GET_STR, REQUIRED_ARG, 0, 0,
     0, 0, 0, 0},
    {"incremental-dir", OPT_XTRA_INCREMENTAL_DIR,
     "(for --prepare): apply .delta files and logfile in the specified "
     "directory.",
     (G_PTR *) &xtrabackup_incremental_dir,
     (G_PTR *) &xtrabackup_incremental_dir, 0, GET_STR, REQUIRED_ARG, 0, 0, 0,
     0, 0, 0},
    {"tables", OPT_XTRA_TABLES, "filtering by regexp for table names.",
     (G_PTR *) &xtrabackup_tables, (G_PTR *) &xtrabackup_tables, 0, GET_STR,
     REQUIRED_ARG, 0, 0, 0, 0, 0, 0},
    {"tables_file", OPT_XTRA_TABLES_FILE,
     "filtering by list of the exact database.table name in the file.",
     (G_PTR *) &xtrabackup_tables_file, (G_PTR *) &xtrabackup_tables_file, 0,
     GET_STR, REQUIRED_ARG, 0, 0, 0, 0, 0, 0},
    {"databases", OPT_XTRA_DATABASES, "filtering by list of databases.",
     (G_PTR *) &xtrabackup_databases, (G_PTR *) &xtrabackup_databases, 0,
     GET_STR, REQUIRED_ARG, 0, 0, 0, 0, 0, 0},
    {"databases_file", OPT_XTRA_DATABASES_FILE,
     "filtering by list of databases in the file.",
     (G_PTR *) &xtrabackup_databases_file,
     (G_PTR *) &xtrabackup_databases_file, 0, GET_STR, REQUIRED_ARG, 0, 0, 0,
     0, 0, 0},
    {"tables-exclude", OPT_XTRA_TABLES_EXCLUDE,
     "filtering by regexp for table names. "
     "Operates the same way as --tables, but matched names are excluded from "
     "backup. "
     "Note that this option has a higher priority than --tables.",
     (G_PTR *) &xtrabackup_tables_exclude,
     (G_PTR *) &xtrabackup_tables_exclude, 0, GET_STR, REQUIRED_ARG, 0, 0, 0,
     0, 0, 0},
    {"databases-exclude", OPT_XTRA_DATABASES_EXCLUDE,
     "Excluding databases based on name, "
     "Operates the same way as --databases, but matched names are excluded "
     "from backup. "
     "Note that this option has a higher priority than --databases.",
     (G_PTR *) &xtrabackup_databases_exclude,
     (G_PTR *) &xtrabackup_databases_exclude, 0, GET_STR, REQUIRED_ARG, 0, 0,
     0, 0, 0, 0},

    {"stream", OPT_XTRA_STREAM,
     "Stream all backup files to the standard output "
     "in the specified format."
     "Supported format is 'mbstream' or 'xbstream'.",
     (G_PTR *) &xtrabackup_stream_str, (G_PTR *) &xtrabackup_stream_str, 0,
     GET_STR, REQUIRED_ARG, 0, 0, 0, 0, 0, 0},

    {"compress", OPT_XTRA_COMPRESS,
     "Compress individual backup files using the "
     "specified compression algorithm. Currently the only supported algorithm "
     "is 'quicklz'. It is also the default algorithm, i.e. the one used when "
     "--compress is used without an argument.",
     (G_PTR *) &xtrabackup_compress_alg, (G_PTR *) &xtrabackup_compress_alg, 0,
     GET_STR, OPT_ARG, 0, 0, 0, 0, 0, 0},

    {"compress-threads", OPT_XTRA_COMPRESS_THREADS,
     "Number of threads for parallel data compression. The default value is "
     "1.",
     (G_PTR *) &xtrabackup_compress_threads,
     (G_PTR *) &xtrabackup_compress_threads, 0, GET_UINT, REQUIRED_ARG, 1, 1,
     UINT_MAX, 0, 0, 0},

    {"compress-chunk-size", OPT_XTRA_COMPRESS_CHUNK_SIZE,
     "Size of working buffer(s) for compression threads in bytes. The default "
     "value is 64K.",
     (G_PTR *) &xtrabackup_compress_chunk_size,
     (G_PTR *) &xtrabackup_compress_chunk_size, 0, GET_ULL, REQUIRED_ARG,
     (1 << 16), 1024, ULONGLONG_MAX, 0, 0, 0},

    {"incremental-force-scan", OPT_XTRA_INCREMENTAL_FORCE_SCAN,
     "Perform a full-scan incremental backup even in the presence of changed "
     "page bitmap data",
     (G_PTR *) &xtrabackup_incremental_force_scan,
     (G_PTR *) &xtrabackup_incremental_force_scan, 0, GET_BOOL, NO_ARG, 0, 0,
     0, 0, 0, 0},

    {"close_files", OPT_CLOSE_FILES,
     "do not keep files opened. Use at your own "
     "risk.",
     (G_PTR *) &xb_close_files, (G_PTR *) &xb_close_files, 0, GET_BOOL, NO_ARG,
     0, 0, 0, 0, 0, 0},

    {"core-file", OPT_CORE_FILE, "Write core on fatal signals", 0, 0, 0,
     GET_NO_ARG, NO_ARG, 0, 0, 0, 0, 0, 0},

    {"copy-back", OPT_COPY_BACK,
     "Copy all the files in a previously made "
     "backup from the backup directory to their original locations.",
     (uchar *) &xtrabackup_copy_back, (uchar *) &xtrabackup_copy_back, 0,
     GET_BOOL, NO_ARG, 0, 0, 0, 0, 0, 0},

    {"move-back", OPT_MOVE_BACK,
     "Move all the files in a previously made "
     "backup from the backup directory to the actual datadir location. "
     "Use with caution, as it removes backup files.",
     (uchar *) &xtrabackup_move_back, (uchar *) &xtrabackup_move_back, 0,
     GET_BOOL, NO_ARG, 0, 0, 0, 0, 0, 0},

    {"galera-info", OPT_GALERA_INFO,
     "This options creates the "
     "xtrabackup_galera_info file which contains the local node state at "
     "the time of the backup. Option should be used when performing the "
     "backup of MariaDB Galera Cluster. Has no effect when backup locks "
     "are used to create the backup.",
     (uchar *) &opt_galera_info, (uchar *) &opt_galera_info, 0, GET_BOOL,
     NO_ARG, 0, 0, 0, 0, 0, 0},

    {"slave-info", OPT_SLAVE_INFO,
     "This option is useful when backing "
     "up a replication slave server. It prints the binary log position "
     "and name of the master server. It also writes this information to "
     "the \"xtrabackup_slave_info\" file as a \"CHANGE MASTER\" command. "
     "A new slave for this master can be set up by starting a slave server "
     "on this backup and issuing a \"CHANGE MASTER\" command with the "
     "binary log position saved in the \"xtrabackup_slave_info\" file.",
     (uchar *) &opt_slave_info, (uchar *) &opt_slave_info, 0, GET_BOOL, NO_ARG,
     0, 0, 0, 0, 0, 0},

    {"no-lock", OPT_NO_LOCK,
     "Use this option to disable table lock "
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
     (uchar *) &opt_no_lock, (uchar *) &opt_no_lock, 0, GET_BOOL, NO_ARG, 0, 0,
     0, 0, 0, 0},

    {"safe-slave-backup", OPT_SAFE_SLAVE_BACKUP,
     "Stop slave SQL thread "
     "and wait to start backup until Slave_open_temp_tables in "
     "\"SHOW STATUS\" is zero. If there are no open temporary tables, "
     "the backup will take place, otherwise the SQL thread will be "
     "started and stopped until there are no open temporary tables. "
     "The backup will fail if Slave_open_temp_tables does not become "
     "zero after --safe-slave-backup-timeout seconds. The slave SQL "
     "thread will be restarted when the backup finishes.",
     (uchar *) &opt_safe_slave_backup, (uchar *) &opt_safe_slave_backup, 0,
     GET_BOOL, NO_ARG, 0, 0, 0, 0, 0, 0},

    {"rsync", OPT_RSYNC,
     "Uses the rsync utility to optimize local file "
     "transfers. When this option is specified, innobackupex uses rsync "
     "to copy all non-InnoDB files instead of spawning a separate cp for "
     "each file, which can be much faster for servers with a large number "
     "of databases or tables.  This option cannot be used together with "
     "--stream.",
     (uchar *) &opt_rsync, (uchar *) &opt_rsync, 0, GET_BOOL, NO_ARG, 0, 0, 0,
     0, 0, 0},

    {"force-non-empty-directories", OPT_FORCE_NON_EMPTY_DIRS,
     "This "
     "option, when specified, makes --copy-back or --move-back transfer "
     "files to non-empty directories. Note that no existing files will be "
     "overwritten. If --copy-back or --move-back has to copy a file from "
     "the backup directory which already exists in the destination "
     "directory, it will still fail with an error.",
     (uchar *) &opt_force_non_empty_dirs, (uchar *) &opt_force_non_empty_dirs,
     0, GET_BOOL, NO_ARG, 0, 0, 0, 0, 0, 0},

    {"no-version-check", OPT_NO_VERSION_CHECK,
     "This option disables the "
     "version check which is enabled by the --version-check option.",
     (uchar *) &opt_noversioncheck, (uchar *) &opt_noversioncheck, 0, GET_BOOL,
     NO_ARG, 0, 0, 0, 0, 0, 0},

    {"no-backup-locks", OPT_NO_BACKUP_LOCKS,
     "This option controls if "
     "backup locks should be used instead of FLUSH TABLES WITH READ LOCK "
     "on the backup stage. The option has no effect when backup locks are "
     "not supported by the server. This option is enabled by default, "
     "disable with --no-backup-locks.",
     (uchar *) &opt_no_backup_locks, (uchar *) &opt_no_backup_locks, 0,
     GET_BOOL, NO_ARG, 0, 0, 0, 0, 0, 0},

    {"decompress", OPT_DECOMPRESS,
     "Decompresses all files with the .qp "
     "extension in a backup previously made with the --compress option.",
     (uchar *) &opt_decompress, (uchar *) &opt_decompress, 0, GET_BOOL, NO_ARG,
     0, 0, 0, 0, 0, 0},

    {"user", 'u',
     "This option specifies the MySQL username used "
     "when connecting to the server, if that's not the current user. "
     "The option accepts a string argument. See mysql --help for details.",
     (uchar *) &opt_user, (uchar *) &opt_user, 0, GET_STR, REQUIRED_ARG, 0, 0,
     0, 0, 0, 0},

    {"host", 'H',
     "This option specifies the host to use when "
     "connecting to the database server with TCP/IP.  The option accepts "
     "a string argument. See mysql --help for details.",
     (uchar *) &opt_host, (uchar *) &opt_host, 0, GET_STR, REQUIRED_ARG, 0, 0,
     0, 0, 0, 0},

    {"port", 'P',
     "This option specifies the port to use when "
     "connecting to the database server with TCP/IP.  The option accepts "
     "a string argument. See mysql --help for details.",
     &opt_port, &opt_port, 0, GET_UINT, REQUIRED_ARG, 0, 0, 0, 0, 0, 0},

    {"password", 'p',
     "This option specifies the password to use "
     "when connecting to the database. It accepts a string argument.  "
     "See mysql --help for details.",
     0, 0, 0, GET_STR, REQUIRED_ARG, 0, 0, 0, 0, 0, 0},

    {"protocol", OPT_PROTOCOL,
     "The protocol to use for connection (tcp, socket, pipe, memory).", 0, 0,
     0, GET_STR, REQUIRED_ARG, 0, 0, 0, 0, 0, 0},

    {"socket", 'S',
     "This option specifies the socket to use when "
     "connecting to the local database server with a UNIX domain socket.  "
     "The option accepts a string argument. See mysql --help for details.",
     (uchar *) &opt_socket, (uchar *) &opt_socket, 0, GET_STR, REQUIRED_ARG, 0,
     0, 0, 0, 0, 0},

    {"incremental-history-name", OPT_INCREMENTAL_HISTORY_NAME,
     "This option specifies the name of the backup series stored in the "
     "PERCONA_SCHEMA.xtrabackup_history history record to base an "
     "incremental backup on. Xtrabackup will search the history table "
     "looking for the most recent (highest innodb_to_lsn), successful "
     "backup in the series and take the to_lsn value to use as the "
     "starting lsn for the incremental backup. This will be mutually "
     "exclusive with --incremental-history-uuid, --incremental-basedir "
     "and --incremental-lsn. If no valid lsn can be found (no series by "
     "that name, no successful backups by that name), an error will be returned."
     " It is used with the --incremental option.",
     (uchar *) &opt_incremental_history_name,
     (uchar *) &opt_incremental_history_name, 0, GET_STR, REQUIRED_ARG, 0, 0,
     0, 0, 0, 0},

    {"incremental-history-uuid", OPT_INCREMENTAL_HISTORY_UUID,
     "This option specifies the UUID of the specific history record "
     "stored in the PERCONA_SCHEMA.xtrabackup_history to base an "
     "incremental backup on. --incremental-history-name, "
     "--incremental-basedir and --incremental-lsn. If no valid lsn can be "
     "found (no success record with that uuid), an error will be returned."
     " It is used with the --incremental option.",
     (uchar *) &opt_incremental_history_uuid,
     (uchar *) &opt_incremental_history_uuid, 0, GET_STR, REQUIRED_ARG, 0, 0,
     0, 0, 0, 0},

    {"remove-original", OPT_REMOVE_ORIGINAL,
     "Remove .qp files after decompression.", (uchar *) &opt_remove_original,
     (uchar *) &opt_remove_original, 0, GET_BOOL, NO_ARG, 0, 0, 0, 0, 0, 0},

    {"ftwrl-wait-query-type", OPT_LOCK_WAIT_QUERY_TYPE,
     "This option specifies which types of queries are allowed to complete "
     "before innobackupex will issue the global lock. Default is all.",
     (uchar *) &opt_lock_wait_query_type, (uchar *) &opt_lock_wait_query_type,
     &query_type_typelib, GET_ENUM, REQUIRED_ARG, QUERY_TYPE_ALL, 0, 0, 0, 0,
     0},

    {"kill-long-query-type", OPT_KILL_LONG_QUERY_TYPE,
     "This option specifies which types of queries should be killed to "
     "unblock the global lock. Default is \"all\".",
     (uchar *) &opt_kill_long_query_type, (uchar *) &opt_kill_long_query_type,
     &query_type_typelib, GET_ENUM, REQUIRED_ARG, QUERY_TYPE_SELECT, 0, 0, 0,
     0, 0},

    {"history", OPT_HISTORY,
     "This option enables the tracking of backup history in the "
     "PERCONA_SCHEMA.xtrabackup_history table. An optional history "
     "series name may be specified that will be placed with the history "
     "record for the current backup being taken.",
     NULL, NULL, 0, GET_STR, OPT_ARG, 0, 0, 0, 0, 0, 0},

    {"kill-long-queries-timeout", OPT_KILL_LONG_QUERIES_TIMEOUT,
     "This option specifies the number of seconds innobackupex waits "
     "between starting FLUSH TABLES WITH READ LOCK and killing those "
     "queries that block it. Default is 0 seconds, which means "
     "innobackupex will not attempt to kill any queries.",
     (uchar *) &opt_kill_long_queries_timeout,
     (uchar *) &opt_kill_long_queries_timeout, 0, GET_UINT, REQUIRED_ARG, 0, 0,
     0, 0, 0, 0},

    {"ftwrl-wait-timeout", OPT_LOCK_WAIT_TIMEOUT,
     "This option specifies time in seconds that innobackupex should wait "
     "for queries that would block FTWRL before running it. If there are "
     "still such queries when the timeout expires, innobackupex terminates "
     "with an error. Default is 0, in which case innobackupex does not "
     "wait for queries to complete and starts FTWRL immediately.",
     (uchar *) &opt_lock_wait_timeout, (uchar *) &opt_lock_wait_timeout, 0,
     GET_UINT, REQUIRED_ARG, 0, 0, 0, 0, 0, 0},

    {"ftwrl-wait-threshold", OPT_LOCK_WAIT_THRESHOLD,
     "This option specifies the query run time threshold which is used by "
     "innobackupex to detect long-running queries with a non-zero value "
     "of --ftwrl-wait-timeout. FTWRL is not started until such "
     "long-running queries exist. This option has no effect if "
     "--ftwrl-wait-timeout is 0. Default value is 60 seconds.",
     (uchar *) &opt_lock_wait_threshold, (uchar *) &opt_lock_wait_threshold, 0,
     GET_UINT, REQUIRED_ARG, 60, 0, 0, 0, 0, 0},


    {"safe-slave-backup-timeout", OPT_SAFE_SLAVE_BACKUP_TIMEOUT,
     "How many seconds --safe-slave-backup should wait for "
     "Slave_open_temp_tables to become zero. (default 300)",
     (uchar *) &opt_safe_slave_backup_timeout,
     (uchar *) &opt_safe_slave_backup_timeout, 0, GET_UINT, REQUIRED_ARG, 300,
     0, 0, 0, 0, 0},

    {"binlog-info", OPT_BINLOG_INFO,
     "This option controls how backup should retrieve server's binary log "
     "coordinates corresponding to the backup. Possible values are OFF, ON, "
     "LOCKLESS and AUTO.",
     &opt_binlog_info, &opt_binlog_info, &binlog_info_typelib, GET_ENUM,
     OPT_ARG, BINLOG_INFO_AUTO, 0, 0, 0, 0, 0},

    {"secure-auth", OPT_XB_SECURE_AUTH,
     "Refuse client connecting to server if it"
     " uses old (pre-4.1.1) protocol.",
     &opt_secure_auth, &opt_secure_auth, 0, GET_BOOL, NO_ARG, 1, 0, 0, 0, 0,
     0},

    {"log-innodb-page-corruption", OPT_XB_IGNORE_INNODB_PAGE_CORRUPTION,
     "Continue backup if innodb corrupted pages are found. The pages are "
     "logged in " MB_CORRUPTED_PAGES_FILE
     " and backup is finished with error. "
     "--prepare will try to fix corrupted pages. If " MB_CORRUPTED_PAGES_FILE
     " exists after --prepare in base backup directory, backup still contains "
     "corrupted pages and can not be considered as consistent.",
     &opt_log_innodb_page_corruption, &opt_log_innodb_page_corruption, 0,
     GET_BOOL, NO_ARG, 0, 0, 0, 0, 0, 0},

#define MYSQL_CLIENT
#include "sslopt-longopts.h"
#undef MYSQL_CLIENT

    {0, 0, 0, 0, 0, 0, GET_NO_ARG, NO_ARG, 0, 0, 0, 0, 0, 0}};

uint xb_client_options_count = array_elements(xb_client_options);

#ifndef DBUG_OFF
/** Parameters to DBUG */
static const char *dbug_option;
#endif

struct my_option xb_server_options[] =
{
  {"datadir", 'h', "Path to the database root.", (G_PTR*) &mysql_data_home,
   (G_PTR*) &mysql_data_home, 0, GET_STR, REQUIRED_ARG, 0, 0, 0, 0, 0, 0},
  {"tmpdir", 't',
   "Path for temporary files. Several paths may be specified, separated by a "
#if defined(__WIN__) || defined(OS2) || defined(__NETWARE__)
   "semicolon (;)"
#else
   "colon (:)"
#endif
   ", in this case they are used in a round-robin fashion.",
   (G_PTR*) &opt_mysql_tmpdir,
   (G_PTR*) &opt_mysql_tmpdir, 0, GET_STR, REQUIRED_ARG, 0, 0, 0, 0, 0, 0},
  {"parallel", OPT_XTRA_PARALLEL,
   "Number of threads to use for parallel datafiles transfer. "
   "The default value is 1.",
   (G_PTR*) &xtrabackup_parallel, (G_PTR*) &xtrabackup_parallel, 0, GET_INT,
   REQUIRED_ARG, 1, 1, INT_MAX, 0, 0, 0},

  {"extended_validation", OPT_XTRA_EXTENDED_VALIDATION,
   "Enable extended validation for Innodb data pages during backup phase. "
   "Will slow down backup considerably, in case encryption is used. "
   "May fail if tables are created during the backup.",
   (G_PTR*)&opt_extended_validation,
   (G_PTR*)&opt_extended_validation,
   0, GET_BOOL, NO_ARG, FALSE, 0, 0, 0, 0, 0},

  {"encrypted_backup", OPT_XTRA_ENCRYPTED_BACKUP,
   "In --backup, assume that nonzero key_version implies that the page"
   " is encrypted. Use --backup --skip-encrypted-backup to allow"
   " copying unencrypted that were originally created before MySQL 5.1.48.",
   (G_PTR*)&opt_encrypted_backup,
   (G_PTR*)&opt_encrypted_backup,
   0, GET_BOOL, NO_ARG, TRUE, 0, 0, 0, 0, 0},

  {"log", OPT_LOG, "Ignored option for MySQL option compatibility",
   (G_PTR*) &log_ignored_opt, (G_PTR*) &log_ignored_opt, 0,
   GET_STR, OPT_ARG, 0, 0, 0, 0, 0, 0},

  {"log_bin", OPT_LOG, "Base name for the log sequence",
   &opt_log_bin, &opt_log_bin, 0, GET_STR, OPT_ARG, 0, 0, 0, 0, 0, 0},

  {"innodb", OPT_INNODB, "Ignored option for MySQL option compatibility",
   (G_PTR*) &innobase_ignored_opt, (G_PTR*) &innobase_ignored_opt, 0,
   GET_STR, OPT_ARG, 0, 0, 0, 0, 0, 0},
#ifdef BTR_CUR_HASH_ADAPT
  {"innodb_adaptive_hash_index", OPT_INNODB_ADAPTIVE_HASH_INDEX,
   "Enable InnoDB adaptive hash index (enabled by default).  "
   "Disable with --skip-innodb-adaptive-hash-index.",
   &btr_search_enabled,
   &btr_search_enabled,
   0, GET_BOOL, NO_ARG, 1, 0, 0, 0, 0, 0},
#endif /* BTR_CUR_HASH_ADAPT */
  {"innodb_autoextend_increment", OPT_INNODB_AUTOEXTEND_INCREMENT,
   "Data file autoextend increment in megabytes",
   (G_PTR*) &sys_tablespace_auto_extend_increment,
   (G_PTR*) &sys_tablespace_auto_extend_increment,
   0, GET_ULONG, REQUIRED_ARG, 8L, 1L, 1000L, 0, 1L, 0},
  {"innodb_data_file_path", OPT_INNODB_DATA_FILE_PATH,
   "Path to individual files and their sizes.", &innobase_data_file_path,
   &innobase_data_file_path, 0, GET_STR, REQUIRED_ARG, 0, 0, 0, 0, 0, 0},
  {"innodb_data_home_dir", OPT_INNODB_DATA_HOME_DIR,
   "The common part for InnoDB table spaces.", &innobase_data_home_dir,
   &innobase_data_home_dir, 0, GET_STR, REQUIRED_ARG, 0, 0, 0, 0, 0, 0},
  {"innodb_doublewrite", OPT_INNODB_DOUBLEWRITE,
   "Enable InnoDB doublewrite buffer during --prepare.",
   (G_PTR*) &innobase_use_doublewrite,
   (G_PTR*) &innobase_use_doublewrite, 0, GET_BOOL, NO_ARG, 0, 0, 0, 0, 0, 0},
  {"innodb_io_capacity", OPT_INNODB_IO_CAPACITY,
   "Number of IOPs the server can do. Tunes the background IO rate",
   (G_PTR*) &srv_io_capacity, (G_PTR*) &srv_io_capacity,
   0, GET_ULONG, OPT_ARG, 200, 100, ~0UL, 0, 0, 0},
  {"innodb_file_io_threads", OPT_INNODB_FILE_IO_THREADS,
   "Number of file I/O threads in InnoDB.", (G_PTR*) &innobase_file_io_threads,
   (G_PTR*) &innobase_file_io_threads, 0, GET_LONG, REQUIRED_ARG, 4, 4, 64, 0,
   1, 0},
  {"innodb_read_io_threads", OPT_INNODB_READ_IO_THREADS,
   "Number of background read I/O threads in InnoDB.", (G_PTR*) &innobase_read_io_threads,
   (G_PTR*) &innobase_read_io_threads, 0, GET_LONG, REQUIRED_ARG, 4, 1, 64, 0,
   1, 0},
  {"innodb_write_io_threads", OPT_INNODB_WRITE_IO_THREADS,
   "Number of background write I/O threads in InnoDB.", (G_PTR*) &innobase_write_io_threads,
   (G_PTR*) &innobase_write_io_threads, 0, GET_LONG, REQUIRED_ARG, 4, 1, 64, 0,
   1, 0},
  {"innodb_file_per_table", OPT_INNODB_FILE_PER_TABLE,
   "Stores each InnoDB table to an .ibd file in the database dir.",
   (G_PTR*) &innobase_file_per_table,
   (G_PTR*) &innobase_file_per_table, 0, GET_BOOL, NO_ARG,
   FALSE, 0, 0, 0, 0, 0},

  {"innodb_flush_method", OPT_INNODB_FLUSH_METHOD,
   "With which method to flush data.", (G_PTR*) &innobase_unix_file_flush_method,
   (G_PTR*) &innobase_unix_file_flush_method, 0, GET_STR, REQUIRED_ARG, 0, 0, 0,
   0, 0, 0},

  {"innodb_log_buffer_size", OPT_INNODB_LOG_BUFFER_SIZE,
   "The size of the buffer which InnoDB uses to write log to the log files on disk.",
   (G_PTR*) &innobase_log_buffer_size, (G_PTR*) &innobase_log_buffer_size, 0,
   GET_LONG, REQUIRED_ARG, 1024*1024L, 256*1024L, LONG_MAX, 0, 1024, 0},
  {"innodb_log_file_size", OPT_INNODB_LOG_FILE_SIZE,
   "Ignored for mysqld option compatibility",
   (G_PTR*) &srv_log_file_size, (G_PTR*) &srv_log_file_size, 0,
   GET_ULL, REQUIRED_ARG, 48 << 20, 1 << 20, log_group_max_size, 0,
   UNIV_PAGE_SIZE_MAX, 0},
  {"innodb_log_files_in_group", OPT_INNODB_LOG_FILES_IN_GROUP,
   "Ignored for mysqld option compatibility",
   &srv_n_log_files, &srv_n_log_files,
   0, GET_LONG, REQUIRED_ARG, 1, 1, 100, 0, 1, 0},
  {"innodb_log_group_home_dir", OPT_INNODB_LOG_GROUP_HOME_DIR,
   "Path to InnoDB log files.", &srv_log_group_home_dir,
   &srv_log_group_home_dir, 0, GET_STR, REQUIRED_ARG, 0, 0, 0, 0, 0, 0},
  {"innodb_max_dirty_pages_pct", OPT_INNODB_MAX_DIRTY_PAGES_PCT,
   "Percentage of dirty pages allowed in bufferpool.", (G_PTR*) &srv_max_buf_pool_modified_pct,
   (G_PTR*) &srv_max_buf_pool_modified_pct, 0, GET_ULONG, REQUIRED_ARG, 90, 0, 100, 0, 0, 0},
  {"innodb_open_files", OPT_INNODB_OPEN_FILES,
   "How many files at the maximum InnoDB keeps open at the same time.",
   (G_PTR*) &innobase_open_files, (G_PTR*) &innobase_open_files, 0,
   GET_LONG, REQUIRED_ARG, 300L, 10L, LONG_MAX, 0, 1L, 0},
  {"innodb_use_native_aio", OPT_INNODB_USE_NATIVE_AIO,
   "Use native AIO if supported on this platform.",
   (G_PTR*) &srv_use_native_aio,
   (G_PTR*) &srv_use_native_aio, 0, GET_BOOL, NO_ARG,
   TRUE, 0, 0, 0, 0, 0},
  {"innodb_page_size", OPT_INNODB_PAGE_SIZE,
   "The universal page size of the database.",
   (G_PTR*) &innobase_page_size, (G_PTR*) &innobase_page_size, 0,
   /* Use GET_LL to support numeric suffixes in 5.6 */
   GET_LL, REQUIRED_ARG,
   (1LL << 14), (1LL << 12), (1LL << UNIV_PAGE_SIZE_SHIFT_MAX), 0, 1L, 0},
  {"innodb_buffer_pool_filename", OPT_INNODB_BUFFER_POOL_FILENAME,
   "Ignored for mysqld option compatibility",
   (G_PTR*) &innobase_buffer_pool_filename,
   (G_PTR*) &innobase_buffer_pool_filename,
   0, GET_STR, REQUIRED_ARG, 0, 0, 0, 0, 0, 0},

#ifndef DBUG_OFF /* unfortunately "debug" collides with existing options */
  {"dbug", '#', "Built in DBUG debugger.",
   &dbug_option, &dbug_option, 0, GET_STR, OPT_ARG,
   0, 0, 0, 0, 0, 0},
#endif

  {"innodb_checksum_algorithm", OPT_INNODB_CHECKSUM_ALGORITHM,
  "The algorithm InnoDB uses for page checksumming. [CRC32, STRICT_CRC32, "
   "INNODB, STRICT_INNODB, NONE, STRICT_NONE]", &srv_checksum_algorithm,
   &srv_checksum_algorithm, &innodb_checksum_algorithm_typelib, GET_ENUM,
   REQUIRED_ARG, SRV_CHECKSUM_ALGORITHM_CRC32, 0, 0, 0, 0, 0},

  {"innodb_undo_directory", OPT_INNODB_UNDO_DIRECTORY,
   "Directory where undo tablespace files live, this path can be absolute.",
   &srv_undo_dir, &srv_undo_dir, 0, GET_STR, REQUIRED_ARG, 0, 0, 0, 0, 0,
   0},

  {"innodb_undo_tablespaces", OPT_INNODB_UNDO_TABLESPACES,
   "Number of undo tablespaces to use.",
   (G_PTR*)&srv_undo_tablespaces, (G_PTR*)&srv_undo_tablespaces,
   0, GET_ULONG, REQUIRED_ARG, 0, 0, 126, 0, 1, 0},

  {"innodb_compression_level", OPT_INNODB_COMPRESSION_LEVEL,
   "Compression level used for zlib compression.",
   (G_PTR*)&page_zip_level, (G_PTR*)&page_zip_level,
   0, GET_UINT, REQUIRED_ARG, 6, 0, 9, 0, 0, 0},

  {"defaults_group", OPT_DEFAULTS_GROUP, "defaults group in config file (default \"mysqld\").",
   (G_PTR*) &defaults_group, (G_PTR*) &defaults_group,
   0, GET_STR, REQUIRED_ARG, 0, 0, 0, 0, 0, 0},

  {"plugin-dir", OPT_PLUGIN_DIR,
   "Server plugin directory. Used to load encryption plugin during 'prepare' phase."
   "Has no effect in the 'backup' phase (plugin directory during backup is the same as server's)",
   &xb_plugin_dir, &xb_plugin_dir,
   0, GET_STR, REQUIRED_ARG, 0, 0, 0, 0, 0, 0 },

  {"innodb-log-checksums", OPT_INNODB_LOG_CHECKSUMS,
   "Whether to require checksums for InnoDB redo log blocks",
   &innodb_log_checksums, &innodb_log_checksums,
   0, GET_BOOL, REQUIRED_ARG, 1, 0, 0, 0, 0, 0 },

  {"open_files_limit", OPT_OPEN_FILES_LIMIT, "the maximum number of file "
   "descriptors to reserve with setrlimit().",
   (G_PTR*) &xb_open_files_limit, (G_PTR*) &xb_open_files_limit, 0, GET_ULONG,
   REQUIRED_ARG, 0, 0, UINT_MAX, 0, 1, 0},

  {"lock-ddl-per-table", OPT_LOCK_DDL_PER_TABLE, "Lock DDL for each table "
   "before backup starts to copy it and until the backup is completed.",
   (uchar*) &opt_lock_ddl_per_table, (uchar*) &opt_lock_ddl_per_table, 0,
   GET_BOOL, NO_ARG, 0, 0, 0, 0, 0, 0},

  {"rocksdb-datadir", OPT_ROCKSDB_DATADIR, "RocksDB data directory."
   "This option is only  used with --copy-back or --move-back option",
  &xb_rocksdb_datadir, &xb_rocksdb_datadir,
  0, GET_STR, REQUIRED_ARG, 0, 0, 0, 0, 0, 0 },

  {"rocksdb-backup", OPT_BACKUP_ROCKSDB, "Backup rocksdb data, if rocksdb plugin is installed."
   "Used only with --backup option. Can be useful for partial backups, to exclude all rocksdb data",
   &xb_backup_rocksdb, &xb_backup_rocksdb,
   0, GET_BOOL, NO_ARG, 1, 0, 0, 0, 0, 0 },

  {"check-privileges", OPT_XTRA_CHECK_PRIVILEGES, "Check database user "
   "privileges fro the backup user",
   &opt_check_privileges, &opt_check_privileges,
   0, GET_BOOL, NO_ARG, 1, 0, 0, 0, 0, 0 },

  {"innodb_force_recovery", OPT_INNODB_FORCE_RECOVERY,
   "(for --prepare): Crash recovery mode (ignores "
   "page corruption; for emergencies only).",
   (G_PTR*)&srv_force_recovery,
   (G_PTR*)&srv_force_recovery,
   0, GET_ULONG, OPT_ARG, 0, 0, SRV_FORCE_IGNORE_CORRUPT, 0, 0, 0},

  { 0, 0, 0, 0, 0, 0, GET_NO_ARG, NO_ARG, 0, 0, 0, 0, 0, 0}
};

uint xb_server_options_count = array_elements(xb_server_options);


static std::set<std::string> tables_for_export;

static void append_export_table(const char *dbname, const char *tablename,
                                bool is_remote, bool set_size)
{
  if(dbname && tablename && !is_remote)
  {
    char buf[3*FN_REFLEN];
    snprintf(buf,sizeof(buf),"%s/%s",dbname, tablename);
    // trim .ibd
    char *p=strrchr(buf, '.');
    if (p) *p=0;

    std::string name=ut_get_name(0, buf);
    /* Strip partition name comment from table name, if any */
    if (ends_with(name.c_str(), "*/"))
    {
      size_t pos= name.rfind("/*");
      if (pos != std::string::npos)
         name.resize(pos);
    }
    tables_for_export.insert(name);
  }
}


#define BOOTSTRAP_FILENAME "mariabackup_prepare_for_export.sql"

static int create_bootstrap_file()
{
  FILE *f= fopen(BOOTSTRAP_FILENAME,"wb");
  if(!f)
   return -1;

  fputs("SET NAMES UTF8;\n",f);
  enumerate_ibd_files(append_export_table);
  for (std::set<std::string>::iterator it = tables_for_export.begin();
       it != tables_for_export.end(); it++)
  {
     const char *tab = it->c_str();
     fprintf(f,
     "BEGIN NOT ATOMIC "
       "DECLARE CONTINUE HANDLER FOR NOT FOUND,SQLEXCEPTION BEGIN END;"
       "FLUSH TABLES %s FOR EXPORT;"
     "END;\n"
     "UNLOCK TABLES;\n",
      tab);
  }
  fclose(f);
  return 0;
}

static int prepare_export()
{
  int err= -1;

  char cmdline[2*FN_REFLEN];
  FILE *outf;

  if (create_bootstrap_file())
    return -1;

  // Process defaults-file , it can have some --lc-language stuff,
  // which is* unfortunately* still necessary to get mysqld up
  if (strncmp(orig_argv1,"--defaults-file=", 16) == 0)
  {
    snprintf(cmdline, sizeof cmdline,
      IF_WIN("\"","") "\"%s\" --mysqld \"%s\""
      " --defaults-extra-file=./backup-my.cnf --defaults-group-suffix=%s --datadir=."
      " --innodb --innodb-fast-shutdown=0 --loose-partition"
      " --innodb_purge_rseg_truncate_frequency=1 --innodb-buffer-pool-size=%llu"
      " --console --skip-log-error --skip-log-bin --bootstrap %s< "
      BOOTSTRAP_FILENAME IF_WIN("\"",""),
      mariabackup_exe,
      orig_argv1, (my_defaults_group_suffix?my_defaults_group_suffix:""),
      xtrabackup_use_memory,
      (srv_force_recovery ? "--innodb-force-recovery=1 " : ""));
  }
  else
  {
    snprintf(cmdline, sizeof cmdline,
      IF_WIN("\"","") "\"%s\" --mysqld"
      " --defaults-file=./backup-my.cnf --defaults-group-suffix=%s --datadir=."
      " --innodb --innodb-fast-shutdown=0 --loose-partition"
      " --innodb_purge_rseg_truncate_frequency=1 --innodb-buffer-pool-size=%llu"
      " --console --log-error= --skip-log-bin --bootstrap %s< "
      BOOTSTRAP_FILENAME IF_WIN("\"",""),
      mariabackup_exe,
      (my_defaults_group_suffix?my_defaults_group_suffix:""),
      xtrabackup_use_memory,
      (srv_force_recovery ? "--innodb-force-recovery=1 " : ""));
  }

  msg("Prepare export : executing %s\n", cmdline);
  fflush(stderr);

  outf= popen(cmdline,"r");
  if (!outf)
    goto end;
  
  char outline[FN_REFLEN];
  while(fgets(outline, sizeof(outline)-1, outf))
    fprintf(stderr,"%s",outline);

  err = pclose(outf);
end:
  unlink(BOOTSTRAP_FILENAME);
  return err;
}


static const char *xb_client_default_groups[]={
   "xtrabackup", "mariabackup",
   "client", "client-server",
   "client-mariadb",
   0, 0, 0
};

static const char *xb_server_default_groups[]={
   "xtrabackup", "mariabackup",
   "mysqld", "server", MYSQL_BASE_VERSION,
   "mariadb", MARIADB_BASE_VERSION,
   "client-server",
   #ifdef WITH_WSREP
   "galera",
   #endif
   0, 0, 0
};

static void print_version(void)
{
  fprintf(stderr, "%s based on MariaDB server %s %s (%s)\n",
      my_progname, MYSQL_SERVER_VERSION, SYSTEM_TYPE, MACHINE_TYPE);
}

static void usage(void)
{
  puts("Open source backup tool for InnoDB and XtraDB\n\
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
You can download full text of the license on http://www.gnu.org/licenses/gpl-2.0.txt\n");

  printf("Usage: %s [--defaults-file=#] [--backup | --prepare | --copy-back | --move-back] [OPTIONS]\n",my_progname);
  print_defaults("my", xb_server_default_groups);
  my_print_help(xb_client_options);
  my_print_help(xb_server_options);
  my_print_variables(xb_server_options);
  my_print_variables(xb_client_options);
}

#define ADD_PRINT_PARAM_OPT(value)              \
  { \
    print_param_str << opt->name << "=" << value << "\n"; \
    param_set.insert(opt->name); \
  }

/************************************************************************
Check if parameter is set in defaults file or via command line argument
@return true if parameter is set. */
bool
check_if_param_set(const char *param)
{
	return param_set.find(param) != param_set.end();
}

my_bool
xb_get_one_option(int optid,
		  const struct my_option *opt __attribute__((unused)),
		  char *argument)
{
  switch(optid) {
  case 'h':
    strmake(mysql_real_data_home,argument, FN_REFLEN - 1);
    mysql_data_home= mysql_real_data_home;

    ADD_PRINT_PARAM_OPT(mysql_real_data_home);
    break;

  case 't':

    ADD_PRINT_PARAM_OPT(opt_mysql_tmpdir);
    break;

  case OPT_INNODB_DATA_HOME_DIR:

    ADD_PRINT_PARAM_OPT(innobase_data_home_dir);
    break;

  case OPT_INNODB_DATA_FILE_PATH:

    ADD_PRINT_PARAM_OPT(innobase_data_file_path);
    break;

  case OPT_INNODB_LOG_GROUP_HOME_DIR:

    ADD_PRINT_PARAM_OPT(srv_log_group_home_dir);
    break;

  case OPT_INNODB_LOG_FILES_IN_GROUP:
  case OPT_INNODB_LOG_FILE_SIZE:
    break;

  case OPT_INNODB_FLUSH_METHOD:

    ADD_PRINT_PARAM_OPT(innobase_unix_file_flush_method);
    break;

  case OPT_INNODB_PAGE_SIZE:

    ADD_PRINT_PARAM_OPT(innobase_page_size);
    break;

  case OPT_INNODB_UNDO_DIRECTORY:

    ADD_PRINT_PARAM_OPT(srv_undo_dir);
    break;

  case OPT_INNODB_UNDO_TABLESPACES:

    ADD_PRINT_PARAM_OPT(srv_undo_tablespaces);
    break;

  case OPT_INNODB_CHECKSUM_ALGORITHM:

    ut_a(srv_checksum_algorithm <= SRV_CHECKSUM_ALGORITHM_STRICT_NONE);

    ADD_PRINT_PARAM_OPT(innodb_checksum_algorithm_names[srv_checksum_algorithm]);
    break;

  case OPT_INNODB_COMPRESSION_LEVEL:
    ADD_PRINT_PARAM_OPT(page_zip_level);
    break;

  case OPT_INNODB_BUFFER_POOL_FILENAME:

    ADD_PRINT_PARAM_OPT(innobase_buffer_pool_filename);
    break;

  case OPT_INNODB_FORCE_RECOVERY:

    if (srv_force_recovery) {
        ADD_PRINT_PARAM_OPT(srv_force_recovery);
    }
    break;

  case OPT_XTRA_TARGET_DIR:
    strmake(xtrabackup_real_target_dir,argument, sizeof(xtrabackup_real_target_dir)-1);
    xtrabackup_target_dir= xtrabackup_real_target_dir;
    break;
  case OPT_XTRA_STREAM:
    if (!strcasecmp(argument, "mbstream") ||
        !strcasecmp(argument, "xbstream"))
      xtrabackup_stream_fmt = XB_STREAM_FMT_XBSTREAM;
    else
    {
      msg("Invalid --stream argument: %s", argument);
      return 1;
    }
    xtrabackup_stream = TRUE;
    break;
  case OPT_XTRA_COMPRESS:
    if (argument == NULL)
      xtrabackup_compress_alg = "quicklz";
    else if (strcasecmp(argument, "quicklz"))
    {
      msg("Invalid --compress argument: %s", argument);
      return 1;
    }
    xtrabackup_compress = TRUE;
    break;
  case OPT_DECOMPRESS:
    opt_decompress = TRUE;
    xtrabackup_decrypt_decompress = true;
    break;
  case (int) OPT_CORE_FILE:
    test_flags |= TEST_CORE_ON_SIGNAL;
    break;
  case OPT_HISTORY:
    if (argument) {
      opt_history = argument;
    } else {
      opt_history = "";
    }
    break;
  case 'p':
    if (argument)
    {
      char *start= argument;
      my_free(opt_password);
      opt_password= my_strdup(argument, MYF(MY_FAE));
      while (*argument) *argument++= 'x';               // Destroy argument
      if (*start)
        start[1]=0 ;
    }
    break;
  case OPT_PROTOCOL:
    if (argument)
    {
      if ((opt_protocol= find_type_with_warning(argument, &sql_protocol_typelib,
                                                opt->name)) <= 0)
      {
        sf_leaking_memory= 1; /* no memory leak reports here */
        exit(1);
      }
    }
    break;
#define MYSQL_CLIENT
#include "sslopt-case.h"
#undef MYSQL_CLIENT

  case '?':
    usage();
    exit(EXIT_SUCCESS);
    break;
  case 'v':
    print_version();
    exit(EXIT_SUCCESS);
    break;
  default:
    break;
  }
  return 0;
}

static my_bool
innodb_init_param(void)
{
	srv_is_being_started = TRUE;
	/* === some variables from mysqld === */
	memset((G_PTR) &mysql_tmpdir_list, 0, sizeof(mysql_tmpdir_list));

	if (init_tmpdir(&mysql_tmpdir_list, opt_mysql_tmpdir))
		die("init_tmpdir() failed");
	xtrabackup_tmpdir = my_tmpdir(&mysql_tmpdir_list);
	/* dummy for initialize all_charsets[] */
	get_charset_name(0);

	srv_page_size = 0;
	srv_page_size_shift = 0;

	if (innobase_page_size != (1LL << 14)) {
		int n_shift = (int)get_bit_shift((ulint) innobase_page_size);

		if (n_shift >= 12 && n_shift <= UNIV_PAGE_SIZE_SHIFT_MAX) {
			srv_page_size_shift = n_shift;
			srv_page_size = 1 << n_shift;
			msg("InnoDB: The page size of the "
			    "database is set to %lu.", srv_page_size);
		} else {
			die("invalid value of "
			    "innobase_page_size: %lld", innobase_page_size);
		}
	} else {
		srv_page_size_shift = 14;
		srv_page_size = (1 << srv_page_size_shift);
	}

	/* Check that values don't overflow on 32-bit systems. */
	if (sizeof(ulint) == 4) {
		if (xtrabackup_use_memory > UINT_MAX32) {
			msg("mariabackup: use-memory can't be over 4GB"
			    " on 32-bit systems");
		}
	}

	static char default_path[2] = { FN_CURLIB, 0 };
	fil_path_to_mysql_datadir = default_path;

	/* Set InnoDB initialization parameters according to the values
	read from MySQL .cnf file */

	if (xtrabackup_backup) {
		msg("mariabackup: using the following InnoDB configuration:");
	} else {
		msg("mariabackup: using the following InnoDB configuration "
		    "for recovery:");
	}

	/*--------------- Data files -------------------------*/

	/* The default dir for data files is the datadir of MySQL */

	srv_data_home = (xtrabackup_backup && innobase_data_home_dir
			 ? innobase_data_home_dir : default_path);
	msg("innodb_data_home_dir = %s", srv_data_home);

	/* Set default InnoDB data file size to 10 MB and let it be
  	auto-extending. Thus users can use InnoDB in >= 4.0 without having
	to specify any startup options. */

	if (!innobase_data_file_path) {
  		innobase_data_file_path = (char*) "ibdata1:10M:autoextend";
	}
	msg("innodb_data_file_path = %s",
	    innobase_data_file_path);

	/* This is the first time univ_page_size is used.
	It was initialized to 16k pages before srv_page_size was set */
	univ_page_size.copy_from(
		page_size_t(srv_page_size, srv_page_size, false));

	srv_sys_space.set_space_id(TRX_SYS_SPACE);
	srv_sys_space.set_name("innodb_system");
	srv_sys_space.set_path(srv_data_home);
	srv_sys_space.set_flags(FSP_FLAGS_PAGE_SSIZE());

	if (!srv_sys_space.parse_params(innobase_data_file_path, true)) {
		goto error;
	}

	/* -------------- Log files ---------------------------*/

	/* The default dir for log files is the datadir of MySQL */

	if (!(xtrabackup_backup && srv_log_group_home_dir)) {
		srv_log_group_home_dir = default_path;
	}
	if (xtrabackup_prepare && xtrabackup_incremental_dir) {
		srv_log_group_home_dir = xtrabackup_incremental_dir;
	}
	msg("innodb_log_group_home_dir = %s",
	    srv_log_group_home_dir);

	os_normalize_path(srv_log_group_home_dir);

	if (strchr(srv_log_group_home_dir, ';')) {
		msg("syntax error in innodb_log_group_home_dir, ");
		goto error;
	}

	srv_adaptive_flushing = FALSE;
	srv_file_format = 1; /* Barracuda */
	srv_max_file_format_at_startup = UNIV_FORMAT_MIN; /* on */
	/* --------------------------------------------------*/

	srv_file_flush_method_str = innobase_unix_file_flush_method;

	srv_log_buffer_size = (ulint) innobase_log_buffer_size;

        /* We set srv_pool_size here in units of 1 kB. InnoDB internally
        changes the value so that it becomes the number of database pages. */

	srv_buf_pool_size = (ulint) xtrabackup_use_memory;
	srv_buf_pool_chunk_unit = srv_buf_pool_size;
	srv_buf_pool_instances = 1;

	srv_n_file_io_threads = (ulint) innobase_file_io_threads;
	srv_n_read_io_threads = (ulint) innobase_read_io_threads;
	srv_n_write_io_threads = (ulint) innobase_write_io_threads;

	srv_use_doublewrite_buf = (ibool) innobase_use_doublewrite;

	row_rollback_on_timeout = (ibool) innobase_rollback_on_timeout;

	srv_file_per_table = (my_bool) innobase_file_per_table;

        srv_locks_unsafe_for_binlog = (ibool) innobase_locks_unsafe_for_binlog;

	srv_max_n_open_files = (ulint) innobase_open_files;
	srv_innodb_status = (ibool) innobase_create_status_file;

	srv_print_verbose_log = verbose ? 2 : 1;

	/* Store the default charset-collation number of this MySQL
	installation */

	/* We cannot treat characterset here for now!! */
	data_mysql_default_charset_coll = (ulint)default_charset_info->number;

	ut_a(DATA_MYSQL_BINARY_CHARSET_COLL == my_charset_bin.number);

	//innobase_commit_concurrency_init_default();

	/* Since we in this module access directly the fields of a trx
        struct, and due to different headers and flags it might happen that
	mutex_t has a different size in this module and in InnoDB
	modules, we check at run time that the size is the same in
	these compilation modules. */

	/* On 5.5+ srv_use_native_aio is TRUE by default. It is later reset
	if it is not supported by the platform in
	innobase_start_or_create_for_mysql(). As we don't call it in xtrabackup,
	we have to duplicate checks from that function here. */

#ifdef _WIN32
	srv_use_native_aio = TRUE;

#elif defined(LINUX_NATIVE_AIO)

	if (srv_use_native_aio) {
		msg("InnoDB: Using Linux native AIO");
	}
#else
	/* Currently native AIO is supported only on windows and linux
	and that also when the support is compiled in. In all other
	cases, we ignore the setting of innodb_use_native_aio. */
	srv_use_native_aio = FALSE;

#endif

	/* Assign the default value to srv_undo_dir if it's not specified, as
	my_getopt does not support default values for string options. We also
	ignore the option and override innodb_undo_directory on --prepare,
	because separate undo tablespaces are copied to the root backup
	directory. */

	if (!srv_undo_dir || !xtrabackup_backup) {
		srv_undo_dir = (char*) ".";
	}

	compile_time_assert(SRV_FORCE_IGNORE_CORRUPT == 1);

	/*
	 * This option can be read both from the command line, and the
	 * defaults file. The assignment should account for both cases,
	 * and for "--innobackupex". Since the command line argument is
	 * parsed after the defaults file, it takes precedence.
	 */
	if (xtrabackup_innodb_force_recovery) {
		srv_force_recovery = xtrabackup_innodb_force_recovery;
	}

	if (srv_force_recovery >= SRV_FORCE_IGNORE_CORRUPT) {
		if (!xtrabackup_prepare) {
			msg("mariabackup: The option \"innodb_force_recovery\""
			    " should only be used with \"%s\".",
			    (innobackupex_mode ? "--apply-log" : "--prepare"));
			goto error;
		} else {
			msg("innodb_force_recovery = %lu", srv_force_recovery);
		}
	}

	return(FALSE);

error:
	msg("innodb_init_param(): Error occured.");
	return(TRUE);
}

static bool innodb_init()
{
	dberr_t err = innobase_start_or_create_for_mysql();
	if (err != DB_SUCCESS) {
		die("mariabackup: innodb_init() returned %d (%s).",
		    err, ut_strerr(err));
	}

	return(FALSE);
}

/* ================= common ================= */

/***********************************************************************
Read backup meta info.
@return TRUE on success, FALSE on failure. */
static
my_bool
xtrabackup_read_metadata(char *filename)
{
	FILE	*fp;
	my_bool	 r = TRUE;

	fp = fopen(filename,"r");
	if(!fp) {
		msg("Error: cannot open %s", filename);
		return(FALSE);
	}

	if (fscanf(fp, "backup_type = %29s\n", metadata_type)
	    != 1) {
		r = FALSE;
		goto end;
	}
	/* Use UINT64PF instead of LSN_PF here, as we have to maintain the file
	format. */
	if (fscanf(fp, "from_lsn = " UINT64PF "\n", &metadata_from_lsn)
			!= 1) {
		r = FALSE;
		goto end;
	}
	if (fscanf(fp, "to_lsn = " UINT64PF "\n", &metadata_to_lsn)
			!= 1) {
		r = FALSE;
		goto end;
	}
	if (fscanf(fp, "last_lsn = " UINT64PF "\n", &metadata_last_lsn)
			!= 1) {
		metadata_last_lsn = 0;
	}
	/* Optional fields */

end:
	fclose(fp);

	return(r);
}

/***********************************************************************
Print backup meta info to a specified buffer. */
static
void
xtrabackup_print_metadata(char *buf, size_t buf_len)
{
	/* Use UINT64PF instead of LSN_PF here, as we have to maintain the file
	format. */
	snprintf(buf, buf_len,
		 "backup_type = %s\n"
		 "from_lsn = " UINT64PF "\n"
		 "to_lsn = " UINT64PF "\n"
		 "last_lsn = " UINT64PF "\n",
		 metadata_type,
		 metadata_from_lsn,
		 metadata_to_lsn,
		 metadata_last_lsn);
}

/***********************************************************************
Stream backup meta info to a specified datasink.
@return TRUE on success, FALSE on failure. */
static
my_bool
xtrabackup_stream_metadata(ds_ctxt_t *ds_ctxt)
{
	char		buf[1024];
	size_t		len;
	ds_file_t	*stream;
	MY_STAT		mystat;
	my_bool		rc = TRUE;

	xtrabackup_print_metadata(buf, sizeof(buf));

	len = strlen(buf);

	mystat.st_size = len;
	mystat.st_mtime = my_time(0);

	stream = ds_open(ds_ctxt, XTRABACKUP_METADATA_FILENAME, &mystat);
	if (stream == NULL) {
		msg("Error: cannot open output stream for %s", XTRABACKUP_METADATA_FILENAME);
		return(FALSE);
	}

	if (ds_write(stream, buf, len)) {
		rc = FALSE;
	}

	if (ds_close(stream)) {
		rc = FALSE;
	}

	return(rc);
}

/***********************************************************************
Write backup meta info to a specified file.
@return TRUE on success, FALSE on failure. */
static
my_bool
xtrabackup_write_metadata(const char *filepath)
{
	char		buf[1024];
	size_t		len;
	FILE		*fp;

	xtrabackup_print_metadata(buf, sizeof(buf));

	len = strlen(buf);

	fp = fopen(filepath, "w");
	if(!fp) {
		msg("Error: cannot open %s", filepath);
		return(FALSE);
	}
	if (fwrite(buf, len, 1, fp) < 1) {
		fclose(fp);
		return(FALSE);
	}

	fclose(fp);

	return(TRUE);
}

/***********************************************************************
Read meta info for an incremental delta.
@return TRUE on success, FALSE on failure. */
static my_bool
xb_read_delta_metadata(const char *filepath, xb_delta_info_t *info)
{
	FILE*	fp;
	char	key[51];
	char	value[51];
	my_bool	r			= TRUE;

	/* set defaults */
	ulint page_size = ULINT_UNDEFINED, zip_size = 0;
	info->space_id = ULINT_UNDEFINED;

	fp = fopen(filepath, "r");
	if (!fp) {
		/* Meta files for incremental deltas are optional */
		return(TRUE);
	}

	while (!feof(fp)) {
		if (fscanf(fp, "%50s = %50s\n", key, value) == 2) {
			if (strcmp(key, "page_size") == 0) {
				page_size = strtoul(value, NULL, 10);
			} else if (strcmp(key, "zip_size") == 0) {
				zip_size = strtoul(value, NULL, 10);
			} else if (strcmp(key, "space_id") == 0) {
				info->space_id = strtoul(value, NULL, 10);
			}
		}
	}

	fclose(fp);

	if (page_size == ULINT_UNDEFINED) {
		msg("page_size is required in %s", filepath);
		r = FALSE;
	} else {
		info->page_size = page_size_t(zip_size ? zip_size : page_size,
					      page_size, zip_size != 0);
	}

	if (info->space_id == ULINT_UNDEFINED) {
		msg("mariabackup: Warning: This backup was taken with XtraBackup 2.0.1 "
			"or earlier, some DDL operations between full and incremental "
			"backups may be handled incorrectly");
	}

	return(r);
}

/***********************************************************************
Write meta info for an incremental delta.
@return TRUE on success, FALSE on failure. */
my_bool
xb_write_delta_metadata(const char *filename, const xb_delta_info_t *info)
{
	ds_file_t	*f;
	char		buf[64];
	my_bool		ret;
	size_t		len;
	MY_STAT		mystat;

	snprintf(buf, sizeof(buf),
		 "page_size = " ULINTPF "\n"
		 "zip_size = " ULINTPF " \n"
		 "space_id = " ULINTPF "\n",
		 info->page_size.logical(),
		 info->page_size.is_compressed()
		 ? info->page_size.physical() : 0,
		 info->space_id);
	len = strlen(buf);

	mystat.st_size = len;
	mystat.st_mtime = my_time(0);

	f = ds_open(ds_meta, filename, &mystat);
	if (f == NULL) {
		msg("Error: Can't open output stream for %s",filename);
		return(FALSE);
	}

	ret = (ds_write(f, buf, len) == 0);

	if (ds_close(f)) {
		ret = FALSE;
	}

	return(ret);
}

/* ================= backup ================= */
void
xtrabackup_io_throttling(void)
{
	if (xtrabackup_backup && xtrabackup_throttle && (io_ticket--) < 0) {
		os_event_reset(wait_throttle);
		os_event_wait(wait_throttle);
	}
}

static
my_bool regex_list_check_match(
	const regex_list_t& list,
	const char* name)
{
	regmatch_t tables_regmatch[1];
	for (regex_list_t::const_iterator i = list.begin(), end = list.end();
	     i != end; ++i) {
		const regex_t& regex = *i;
		int regres = regexec(&regex, name, 1, tables_regmatch, 0);

		if (regres != REG_NOMATCH) {
			return(TRUE);
		}
	}
	return(FALSE);
}

static
my_bool
find_filter_in_hashtable(
	const char* name,
	hash_table_t* table,
	xb_filter_entry_t** result
)
{
	xb_filter_entry_t* found = NULL;
	HASH_SEARCH(name_hash, table, ut_fold_string(name),
		    xb_filter_entry_t*,
		    found, (void) 0,
		    !strcmp(found->name, name));

	if (found && result) {
		*result = found;
	}
	return (found != NULL);
}

/************************************************************************
Checks if a given table name matches any of specifications given in
regex_list or tables_hash.

@return TRUE on match or both regex_list and tables_hash are empty.*/
static my_bool
check_if_table_matches_filters(const char *name,
	const regex_list_t& regex_list,
	hash_table_t* tables_hash)
{
	if (regex_list.empty() && !tables_hash) {
		return(FALSE);
	}

	if (regex_list_check_match(regex_list, name)) {
		return(TRUE);
	}

	if (tables_hash && find_filter_in_hashtable(name, tables_hash, NULL)) {
		return(TRUE);
	}

	return FALSE;
}

enum skip_database_check_result {
	DATABASE_SKIP,
	DATABASE_SKIP_SOME_TABLES,
	DATABASE_DONT_SKIP,
	DATABASE_DONT_SKIP_UNLESS_EXPLICITLY_EXCLUDED,
};

/************************************************************************
Checks if a database specified by name should be skipped from backup based on
the --databases, --databases_file or --databases_exclude options.

@return TRUE if entire database should be skipped,
	FALSE otherwise.
*/
static
skip_database_check_result
check_if_skip_database(
	const char* name  /*!< in: path to the database */
)
{
	/* There are some filters for databases, check them */
	xb_filter_entry_t*	database = NULL;

	if (databases_exclude_hash &&
		find_filter_in_hashtable(name, databases_exclude_hash,
					 &database) &&
		(!database->has_tables || !databases_include_hash)) {
		/* Database is found and there are no tables specified,
		   skip entire db. */
		return DATABASE_SKIP;
	}

	if (databases_include_hash) {
		if (!find_filter_in_hashtable(name, databases_include_hash,
					      &database)) {
		/* Database isn't found, skip the database */
			return DATABASE_SKIP;
		} else if (database->has_tables) {
			return DATABASE_SKIP_SOME_TABLES;
		} else {
			return DATABASE_DONT_SKIP_UNLESS_EXPLICITLY_EXCLUDED;
		}
	}

	return DATABASE_DONT_SKIP;
}

/************************************************************************
Checks if a database specified by path should be skipped from backup based on
the --databases, --databases_file or --databases_exclude options.

@return TRUE if the table should be skipped. */
my_bool
check_if_skip_database_by_path(
	const char* path /*!< in: path to the db directory. */
)
{
	if (databases_include_hash == NULL &&
		databases_exclude_hash == NULL) {
		return(FALSE);
	}

	const char* db_name = strrchr(path, OS_PATH_SEPARATOR);
	if (db_name == NULL) {
		db_name = path;
	} else {
		++db_name;
	}

	return check_if_skip_database(db_name) == DATABASE_SKIP;
}

/************************************************************************
Checks if a table specified as a name in the form "database/name" (InnoDB 5.6)
or "./database/name.ibd" (InnoDB 5.5-) should be skipped from backup based on
the --tables or --tables-file options.

@return TRUE if the table should be skipped. */
my_bool
check_if_skip_table(
/******************/
	const char*	name)	/*!< in: path to the table */
{
	char buf[FN_REFLEN];
	const char *dbname, *tbname;
	const char *ptr;
	char *eptr;

	if (regex_exclude_list.empty() &&
		regex_include_list.empty() &&
		tables_include_hash == NULL &&
		tables_exclude_hash == NULL &&
		databases_include_hash == NULL &&
		databases_exclude_hash == NULL) {
		return(FALSE);
	}

	dbname = NULL;
	tbname = name;
	while ((ptr = strchr(tbname, '/')) != NULL) {
		dbname = tbname;
		tbname = ptr + 1;
	}

	if (dbname == NULL) {
		return(FALSE);
	}

	strncpy(buf, dbname, FN_REFLEN - 1);
	buf[FN_REFLEN - 1] = '\0';
	buf[tbname - 1 - dbname] = '\0';

	const skip_database_check_result skip_database =
			check_if_skip_database(buf);
	if (skip_database == DATABASE_SKIP) {
		return (TRUE);
	}

	buf[tbname - 1 - dbname] = '.';

	/* Check if there's a suffix in the table name. If so, truncate it. We
	rely on the fact that a dot cannot be a part of a table name (it is
	encoded by the server with the @NNNN syntax). */
	if ((eptr = strchr(&buf[tbname - dbname], '.')) != NULL) {

		*eptr = '\0';
	}

	/* For partitioned tables first try to match against the regexp
	without truncating the #P#... suffix so we can backup individual
	partitions with regexps like '^test[.]t#P#p5' */
	if (check_if_table_matches_filters(buf, regex_exclude_list,
					   tables_exclude_hash)) {
		return(TRUE);
	}
	if (check_if_table_matches_filters(buf, regex_include_list,
					   tables_include_hash)) {
		return(FALSE);
	}
	if ((eptr = strstr(buf, "#P#")) != NULL) {
		*eptr = 0;

		if (check_if_table_matches_filters(buf, regex_exclude_list,
						   tables_exclude_hash)) {
			return (TRUE);
		}
		if (check_if_table_matches_filters(buf, regex_include_list,
						   tables_include_hash)) {
			return(FALSE);
		}
	}

	if (skip_database == DATABASE_DONT_SKIP_UNLESS_EXPLICITLY_EXCLUDED) {
		/* Database is in include-list, and qualified name wasn't
		   found in any of exclusion filters.*/
		return (FALSE);
	}

	if (skip_database == DATABASE_SKIP_SOME_TABLES ||
		!regex_include_list.empty() ||
		tables_include_hash) {

		/* Include lists are present, but qualified name
		   failed to match any.*/
		return(TRUE);
	}

	return(FALSE);
}

const char*
xb_get_copy_action(const char *dflt)
{
	const char *action;

	if (xtrabackup_stream) {
		if (xtrabackup_compress) {
			action = "Compressing and streaming";
		} else {
			action = "Streaming";
		}
	} else {
		if (xtrabackup_compress) {
			action = "Compressing";
		} else {
			action = dflt;
		}
	}

	return(action);
}


/** Copy innodb data file to the specified destination.

@param[in] node	file node of a tablespace
@param[in] thread_n	thread id, used in the text of diagnostic messages
@param[in] dest_name	destination file name
@param[in] write_filter	write filter to copy data, can be pass-through filter
for full backup, pages filter for incremental backup, etc.

@return FALSE on success and TRUE on error */
static my_bool xtrabackup_copy_datafile(fil_node_t *node, uint thread_n,
                                        const char *dest_name,
                                        const xb_write_filt_t &write_filter,
                                        CorruptedPages &corrupted_pages)
{
	char			 dst_name[FN_REFLEN];
	ds_file_t		*dstfile = NULL;
	xb_fil_cur_t		 cursor;
	xb_fil_cur_result_t	 res;
	xb_write_filt_ctxt_t	 write_filt_ctxt;
	const char		*action;
	xb_read_filt_t		*read_filter;
	my_bool			rc = FALSE;

	/* Get the name and the path for the tablespace. node->name always
	contains the path (which may be absolute for remote tablespaces in
	5.6+). space->name contains the tablespace name in the form
	"./database/table.ibd" (in 5.5-) or "database/table" (in 5.6+). For a
	multi-node shared tablespace, space->name contains the name of the first
	node, but that's irrelevant, since we only need node_name to match them
	against filters, and the shared tablespace is always copied regardless
	of the filters value. */

	const char* const node_name = node->space->name;
	const char* const node_path = node->name;

	if (fil_is_user_tablespace_id(node->space->id)
	    && check_if_skip_table(node_name)) {
		msg(thread_n, "Skipping %s.", node_name);
		return(FALSE);
	}

	memset(&write_filt_ctxt, 0, sizeof(xb_write_filt_ctxt_t));

	bool was_dropped;
	pthread_mutex_lock(&backup_mutex);
	was_dropped = (ddl_tracker.drops.find(node->space->id) != ddl_tracker.drops.end());
	pthread_mutex_unlock(&backup_mutex);
	if (was_dropped) {
		fil_space_close(node->space->name);
		goto skip;
	}

	if (!changed_page_bitmap) {
		read_filter = &rf_pass_through;
	}
	else {
		read_filter = &rf_bitmap;
	}

	res = xb_fil_cur_open(&cursor, read_filter, node, thread_n, ULLONG_MAX);
	if (res == XB_FIL_CUR_SKIP) {
		goto skip;
	} else if (res == XB_FIL_CUR_ERROR) {
		goto error;
	}

	strncpy(dst_name, dest_name ? dest_name : cursor.rel_path,
		sizeof dst_name - 1);
	dst_name[sizeof dst_name - 1] = '\0';

	ut_a(write_filter.process != NULL);

	if (write_filter.init != NULL &&
		!write_filter.init(&write_filt_ctxt, dst_name, &cursor,
			opt_log_innodb_page_corruption ? &corrupted_pages : NULL)) {
		msg (thread_n, "mariabackup: error: failed to initialize page write filter.");
		goto error;
	}

	dstfile = ds_open(ds_data, dst_name, &cursor.statinfo);
	if (dstfile == NULL) {
		msg(thread_n,"mariabackup: error: can't open the destination stream for %s", dst_name);
		goto error;
	}

	action = xb_get_copy_action();

	if (xtrabackup_stream) {
		msg(thread_n, "%s %s", action, node_path);
	} else {
		msg(thread_n, "%s %s to %s", action, node_path, dstfile->path);
	}

	/* The main copy loop */
	while ((res = xb_fil_cur_read(&cursor, corrupted_pages)) ==
		XB_FIL_CUR_SUCCESS) {
		if (!write_filter.process(&write_filt_ctxt, dstfile)) {
			goto error;
		}
	}

	if (res == XB_FIL_CUR_ERROR) {
		goto error;
	}

	if (write_filter.finalize
	    && !write_filter.finalize(&write_filt_ctxt, dstfile)) {
		goto error;
	}

	pthread_mutex_lock(&backup_mutex);
	ddl_tracker.tables_in_backup[node->space->id] = node_name;
	pthread_mutex_unlock(&backup_mutex);

	/* close */
	msg(thread_n,"        ...done");
	xb_fil_cur_close(&cursor);
	if (ds_close(dstfile)) {
		rc = TRUE;
	}
	if (write_filter.deinit) {
		write_filter.deinit(&write_filt_ctxt);
	}
	return(rc);

error:
	xb_fil_cur_close(&cursor);
	if (dstfile != NULL) {
		ds_close(dstfile);
	}
	if (write_filter.deinit) {
		write_filter.deinit(&write_filt_ctxt);;
	}
	msg(thread_n, "mariabackup: xtrabackup_copy_datafile() failed.");
	return(TRUE); /*ERROR*/

skip:

	if (dstfile != NULL) {
		ds_close(dstfile);
	}
	if (write_filter.deinit) {
		write_filter.deinit(&write_filt_ctxt);
	}
	msg(thread_n,"Warning: We assume the  table was dropped during xtrabackup execution and ignore the tablespace %s", node_name);
	return(FALSE);
}

/** Copy redo log blocks to the data sink.
@param start_lsn	buffer start LSN
@param end_lsn		buffer end LSN
@param last		whether we are copying the final part of the log
@return	last scanned LSN
@retval	0	on failure */
static lsn_t xtrabackup_copy_log(lsn_t start_lsn, lsn_t end_lsn, bool last)
{
	lsn_t	scanned_lsn	= start_lsn;
	const byte* log_block = log_sys->buf;
	bool more_data = false;

	for (ulint scanned_checkpoint = 0;
	     scanned_lsn < end_lsn;
	     log_block += OS_FILE_LOG_BLOCK_SIZE) {
		ulint checkpoint = log_block_get_checkpoint_no(log_block);

		if (scanned_checkpoint > checkpoint
		    && scanned_checkpoint - checkpoint >= 0x80000000UL) {
			/* Garbage from a log buffer flush which was made
			before the most recent database recovery */
			msg(0,"checkpoint wrap: " LSN_PF ",%zx,%zx",
				scanned_lsn, scanned_checkpoint, checkpoint);
			break;
		}

		scanned_checkpoint = checkpoint;

		ulint	data_len = log_block_get_data_len(log_block);

		more_data = recv_sys_add_to_parsing_buf(
				log_block,
				scanned_lsn + data_len);

		recv_sys->scanned_lsn = scanned_lsn + data_len;

		if (data_len == OS_FILE_LOG_BLOCK_SIZE) {
			/* We got a full log block. */
			scanned_lsn += data_len;
		} else if (data_len
			   >= OS_FILE_LOG_BLOCK_SIZE - LOG_BLOCK_TRL_SIZE
			   || data_len < LOG_BLOCK_HDR_SIZE) {
			/* We got a garbage block (abrupt end of the log). */
			msg(0,"garbage block: " LSN_PF ",%zu",scanned_lsn, data_len);
			break;
		} else {
			/* We got a partial block (abrupt end of the log). */
			scanned_lsn += data_len;
			break;
		}
	}

	store_t store = STORE_NO;

	if (more_data && recv_parse_log_recs(0, &store, 0, false)) {

		msg("Error: copying the log failed");

		return(0);
	}

	recv_sys_justify_left_parsing_buf();

	log_sys->log.scanned_lsn = scanned_lsn;

	end_lsn = last
		? ut_uint64_align_up(scanned_lsn, OS_FILE_LOG_BLOCK_SIZE)
		: scanned_lsn & ~lsn_t(OS_FILE_LOG_BLOCK_SIZE - 1);

	if (ulint write_size = ulint(end_lsn - start_lsn)) {
		if (srv_encrypt_log) {
			log_crypt(log_sys->buf, start_lsn, write_size);
		}

		if (ds_write(dst_log_file, log_sys->buf, write_size)) {
			msg("Error: write to logfile failed\n");
			return(0);
		}
	}

	return(scanned_lsn);
}

/** Copy redo log until the current end of the log is reached
@param last	whether we are copying the final part of the log
@return	whether the operation failed */
static bool xtrabackup_copy_logfile(bool last = false)
{
	ut_a(dst_log_file != NULL);
	ut_ad(recv_sys != NULL);

	bool overwritten_block = false;
	lsn_t	start_lsn;
	lsn_t	end_lsn;

	recv_sys->parse_start_lsn = log_copy_scanned_lsn;
	recv_sys->scanned_lsn = log_copy_scanned_lsn;

	start_lsn = ut_uint64_align_down(log_copy_scanned_lsn,
					 OS_FILE_LOG_BLOCK_SIZE);
	do {
		end_lsn = start_lsn + RECV_SCAN_SIZE;

		xtrabackup_io_throttling();

		log_mutex_enter();
		lsn_t lsn= start_lsn;
		for (int retries= 0; retries < 100; retries++) {
			if (log_group_read_log_seg(log_sys->buf, &log_sys->log,
						   &lsn, end_lsn)
			    || lsn != start_lsn) {
				break;
			}
			msg("Retrying read of log at LSN=" LSN_PF, lsn);
			my_sleep(1000);
		}

		if (lsn == start_lsn) {
			overwritten_block= !recv_sys->found_corrupt_log
				&& (innodb_log_checksums || log_sys->log.is_encrypted())
				&& log_block_calc_checksum_crc32(log_sys->buf) ==
					log_block_get_checksum(log_sys->buf)
				&& log_block_get_hdr_no(log_sys->buf) >
					log_block_convert_lsn_to_no(start_lsn);
			start_lsn = 0;
		} else {
			mutex_enter(&recv_sys->mutex);
			start_lsn = xtrabackup_copy_log(start_lsn, lsn, last);
			mutex_exit(&recv_sys->mutex);
		}

		log_mutex_exit();

		if (!start_lsn) {
			const char *reason = recv_sys->found_corrupt_log
				? "corrupt log."
				: (overwritten_block
					? "redo log block is overwritten, please increase redo log size with innodb_log_file_size parameter."
					: ((innodb_log_checksums || log_sys->log.is_encrypted())
						? "redo log block checksum does not match."
						: "unknown reason as innodb_log_checksums is switched off and redo"
							" log is not encrypted."));
			die("xtrabackup_copy_logfile() failed: %s", reason);
			return true;
		}
	} while (start_lsn == end_lsn);

	ut_ad(start_lsn == log_sys->log.scanned_lsn);

	msg(">> log scanned up to (" LSN_PF ")", start_lsn);

	/* update global variable*/
	pthread_mutex_lock(&backup_mutex);
	log_copy_scanned_lsn = start_lsn;
	pthread_cond_broadcast(&scanned_lsn_cond);
	pthread_mutex_unlock(&backup_mutex);
	return(false);
}

/**
Wait until redo log copying thread processes given lsn
*/
void backup_wait_for_lsn(lsn_t lsn) {
	bool completed = false;
	pthread_mutex_lock(&backup_mutex);
	do {
		pthread_cond_wait(&scanned_lsn_cond, &backup_mutex);
		completed = log_copy_scanned_lsn >= lsn;
	} while (!completed);
	pthread_mutex_unlock(&backup_mutex);
}

extern lsn_t server_lsn_after_lock;

static os_thread_ret_t log_copying_thread(void*)
{
	/*
	  Initialize mysys thread-specific memory so we can
	  use mysys functions in this thread.
	*/
	my_thread_init();

	for (;;) {
		os_event_reset(log_copying_stop);
		os_event_wait_time_low(log_copying_stop,
				       xtrabackup_log_copy_interval * 1000ULL,
				       0);
		if (xtrabackup_copy_logfile()) {
			break;
		}

		log_mutex_enter();
		bool completed = metadata_to_lsn
			&& metadata_to_lsn <= log_copy_scanned_lsn;
		log_mutex_exit();
		if (completed) {
			break;
		}
	}

	log_copying_running = false;
	my_thread_end();
	os_thread_exit();

	return(0);
}

/* io throttle watching (rough) */
static os_thread_ret_t io_watching_thread(void*)
{
	/* currently, for --backup only */
	ut_a(xtrabackup_backup);

	while (log_copying_running && !metadata_to_lsn) {
		os_thread_sleep(1000000); /*1 sec*/
		io_ticket = xtrabackup_throttle;
		os_event_set(wait_throttle);
	}

	/* stop io throttle */
	xtrabackup_throttle = 0;
	os_event_set(wait_throttle);

	io_watching_thread_running = false;

	os_thread_exit();

	return(0);
}

#ifndef DBUG_OFF
char *dbug_mariabackup_get_val(const char *event, const char *key)
{
	char envvar[FN_REFLEN];
	if (key) {
		snprintf(envvar, sizeof(envvar), "%s_%s", event, key);
		char *slash = strchr(envvar, '/');
		if (slash)
			*slash = '_';
	} else {
		strncpy(envvar, event, sizeof envvar - 1);
		envvar[sizeof envvar - 1] = '\0';
	}
	return getenv(envvar);
}

/*
In debug mode,  execute SQL statement that was passed via environment.
To use this facility, you need to

1. Add code DBUG_EXECUTE_MARIABACKUP_EVENT("my_event_name", key););
  to the code. key is usually a table name
2. Set environment variable my_event_name_$key SQL statement you want to execute
   when event occurs, in DBUG_EXECUTE_IF from above.
   In mtr , you can set environment via 'let' statement (do not use $ as the first char
   for the variable)
3. start mariabackup with --dbug=+d,debug_mariabackup_events
*/
void dbug_mariabackup_event(const char *event,const char *key)
{
	char *sql = dbug_mariabackup_get_val(event, key);
	if (sql && *sql) {
		msg("dbug_mariabackup_event : executing '%s'", sql);
		xb_mysql_query(mysql_connection, sql, false, true);
	}
}
#endif // DBUG_OFF

/**************************************************************************
Datafiles copying thread.*/
static
os_thread_ret_t
data_copy_thread_func(
/*==================*/
	void *arg) /* thread context */
{
	data_thread_ctxt_t	*ctxt = (data_thread_ctxt_t *) arg;
	uint			num = ctxt->num;
	fil_node_t*		node;
	ut_ad(ctxt->corrupted_pages);

	/*
	  Initialize mysys thread-specific memory so we can
	  use mysys functions in this thread.
	*/
	my_thread_init();

	while ((node = datafiles_iter_next(ctxt->it)) != NULL) {
		DBUG_MARIABACKUP_EVENT("before_copy", node->space->name);
		DBUG_EXECUTE_FOR_KEY("wait_innodb_redo_before_copy", node->space->name,
			backup_wait_for_lsn(get_current_lsn(mysql_connection)););
		/* copy the datafile */
		if (xtrabackup_copy_datafile(node, num, NULL,
			xtrabackup_incremental ? wf_incremental : wf_write_through,
			*ctxt->corrupted_pages))
			die("failed to copy datafile.");

		DBUG_MARIABACKUP_EVENT("after_copy", node->space->name);

	}

	pthread_mutex_lock(ctxt->count_mutex);
	(*ctxt->count)--;
	pthread_mutex_unlock(ctxt->count_mutex);

	my_thread_end();
	os_thread_exit();
	OS_THREAD_DUMMY_RETURN;
}

/************************************************************************
Initialize the appropriate datasink(s). Both local backups and streaming in the
'xbstream' format allow parallel writes so we can write directly.

Otherwise (i.e. when streaming in the 'tar' format) we need 2 separate datasinks
for the data stream (and don't allow parallel data copying) and for metainfo
files (including ib_logfile0). The second datasink writes to temporary
files first, and then streams them in a serialized way when closed. */
static void
xtrabackup_init_datasinks(void)
{
	/* Start building out the pipelines from the terminus back */
	if (xtrabackup_stream) {
		/* All streaming goes to stdout */
		ds_data = ds_meta = ds_redo = ds_create(xtrabackup_target_dir,
						        DS_TYPE_STDOUT);
	} else {
		/* Local filesystem */
		ds_data = ds_meta = ds_redo = ds_create(xtrabackup_target_dir,
						        DS_TYPE_LOCAL);
	}

	/* Track it for destruction */
	xtrabackup_add_datasink(ds_data);

	/* Stream formatting */
	if (xtrabackup_stream) {
		ds_ctxt_t	*ds;

	 ut_a(xtrabackup_stream_fmt == XB_STREAM_FMT_XBSTREAM);
	 ds = ds_create(xtrabackup_target_dir, DS_TYPE_XBSTREAM);

		xtrabackup_add_datasink(ds);

		ds_set_pipe(ds, ds_data);
		ds_data = ds;


		ds_redo = ds_meta = ds_data;
	}

	/* Compression for ds_data and ds_redo */
	if (xtrabackup_compress) {
		ds_ctxt_t	*ds;

		/* Use a 1 MB buffer for compressed output stream */
		ds = ds_create(xtrabackup_target_dir, DS_TYPE_BUFFER);
		ds_buffer_set_size(ds, 1024 * 1024);
		xtrabackup_add_datasink(ds);
		ds_set_pipe(ds, ds_data);
		if (ds_data != ds_redo) {
			ds_data = ds;
			ds = ds_create(xtrabackup_target_dir, DS_TYPE_BUFFER);
			ds_buffer_set_size(ds, 1024 * 1024);
			xtrabackup_add_datasink(ds);
			ds_set_pipe(ds, ds_redo);
			ds_redo = ds;
		} else {
			ds_redo = ds_data = ds;
		}

		ds = ds_create(xtrabackup_target_dir, DS_TYPE_COMPRESS);
		xtrabackup_add_datasink(ds);
		ds_set_pipe(ds, ds_data);
		if (ds_data != ds_redo) {
			ds_data = ds;
			ds = ds_create(xtrabackup_target_dir, DS_TYPE_COMPRESS);
			xtrabackup_add_datasink(ds);
			ds_set_pipe(ds, ds_redo);
			ds_redo = ds;
		} else {
			ds_redo = ds_data = ds;
		}
	}
}

/************************************************************************
Destroy datasinks.

Destruction is done in the specific order to not violate their order in the
pipeline so that each datasink is able to flush data down the pipeline. */
static void xtrabackup_destroy_datasinks(void)
{
	for (uint i = actual_datasinks; i > 0; i--) {
		ds_destroy(datasinks[i-1]);
		datasinks[i-1] = NULL;
	}
	ds_data = NULL;
	ds_meta = NULL;
	ds_redo = NULL;
}

#define SRV_MAX_N_PENDING_SYNC_IOS	100

/** Initialize the tablespace cache subsystem. */
static
void
xb_fil_io_init()
{
	fil_init(srv_file_per_table ? 50000 : 5000, LONG_MAX);
	fsp_init();
}

static
Datafile*
xb_new_datafile(const char *name, bool is_remote)
{
	if (is_remote) {
		RemoteDatafile *remote_file = new RemoteDatafile();
		remote_file->set_name(name);
		return(remote_file);
	} else {
		Datafile *file = new Datafile();
		file->set_name(name);
		file->make_filepath(".", name, IBD);
		return(file);
	}
}


/** Load tablespace.

@param[in] dirname directory name of the tablespace to open
@param[in] filname file name of the tablespece to open
@param[in] is_remote true if tablespace file is .isl
@param[in] set_size true if we need to set tablespace size in pages explixitly.
If this parameter is set, the size and free pages limit will not be read
from page 0.
*/
static void xb_load_single_table_tablespace(const char *dirname,
                                            const char *filname,
                                            bool is_remote, bool set_size)
{
	ut_ad(srv_operation == SRV_OPERATION_BACKUP
	      || srv_operation == SRV_OPERATION_RESTORE_DELTA
	      || srv_operation == SRV_OPERATION_RESTORE);
	/* Ignore .isl files on XtraBackup recovery. All tablespaces must be
	local. */
	if (is_remote && srv_operation == SRV_OPERATION_RESTORE_DELTA) {
		return;
	}
	if (check_if_skip_table(filname)) {
		return;
	}

	/* The name ends in .ibd or .isl;
	try opening the file */
	char*	name;
	size_t	dirlen		= dirname == NULL ? 0 : strlen(dirname);
	size_t	namelen		= strlen(filname);
	ulint	pathlen		= dirname == NULL ? namelen + 1: dirlen + namelen + 2;
	lsn_t	flush_lsn;
	dberr_t	err;
	fil_space_t	*space;

	name = static_cast<char*>(ut_malloc_nokey(pathlen));

	if (dirname != NULL) {
		snprintf(name, pathlen, "%s/%s", dirname, filname);
		name[pathlen - 5] = 0;
	} else {
		snprintf(name, pathlen, "%s", filname);
		name[pathlen - 5] = 0;
	}

	Datafile *file = xb_new_datafile(name, is_remote);

	if (file->open_read_only(true) != DB_SUCCESS) {
		die("Can't open datafile %s", name);
	}

	for (int i = 0; i < 10; i++) {
		err = file->validate_first_page(&flush_lsn);
		if (err != DB_CORRUPTION) {
			break;
		}

		my_sleep(1000);
	}

	bool is_empty_file = file->exists() && file->is_empty_file();

	if (err == DB_SUCCESS && file->space_id() != SRV_TMP_SPACE_ID) {
		os_offset_t	n_pages = 0;
		if (set_size) {
			os_offset_t	node_size = os_file_get_size(file->handle());
			ut_a(node_size != (os_offset_t) -1);
			n_pages = node_size / page_size_t(file->flags()).physical();
		}
		space = fil_space_create(
			name, file->space_id(), file->flags(),
			FIL_TYPE_TABLESPACE, NULL/* TODO: crypt_data */);

		ut_a(space != NULL);

		space->add(file->filepath(), OS_FILE_CLOSED, ulint(n_pages),
			   false, false);
		/* by opening the tablespace we forcing node and space objects
		in the cache to be populated with fields from space header */
		fil_space_open(space->name);

		if (srv_operation == SRV_OPERATION_RESTORE_DELTA
		    || xb_close_files) {
			fil_space_close(space->name);
		}
	}

	delete file;

	if (err != DB_SUCCESS && xtrabackup_backup && !is_empty_file) {
		die("Failed to validate first page of the file %s, error %d",name, (int)err);
	}

	ut_free(name);
}

static void xb_load_single_table_tablespace(const std::string &space_name,
                                            bool set_size)
{
  std::string name(space_name);
  bool is_remote= access((name + ".ibd").c_str(), R_OK) != 0;
  const char *extension= is_remote ? ".isl" : ".ibd";
  name.append(extension);
  char buf[FN_REFLEN];
  strncpy(buf, name.c_str(), sizeof buf - 1);
  buf[sizeof buf - 1]= '\0';
  const char *dbname= buf;
  char *p= strchr(buf, '/');
  if (p == 0)
    die("Unexpected tablespace %s filename %s", space_name.c_str(),
        name.c_str());
  ut_a(p);
  *p= 0;
  const char *tablename= p + 1;
  xb_load_single_table_tablespace(dbname, tablename, is_remote, set_size);
}

/** Scan the database directories under the MySQL datadir, looking for
.ibd files and determining the space id in each of them.
@return	DB_SUCCESS or error number */

static dberr_t enumerate_ibd_files(process_single_tablespace_func_t callback)
{
	int		ret;
	char*		dbpath		= NULL;
	ulint		dbpath_len	= 100;
	os_file_dir_t	dir;
	os_file_dir_t	dbdir;
	os_file_stat_t	dbinfo;
	os_file_stat_t	fileinfo;
	dberr_t		err		= DB_SUCCESS;
	size_t len;

	/* The datadir of MySQL is always the default directory of mysqld */

	dir = os_file_opendir(fil_path_to_mysql_datadir, true);

	if (dir == NULL) {

		return(DB_ERROR);
	}

	dbpath = static_cast<char*>(ut_malloc_nokey(dbpath_len));

	/* Scan all directories under the datadir. They are the database
	directories of MySQL. */

	ret = fil_file_readdir_next_file(&err, fil_path_to_mysql_datadir, dir,
					 &dbinfo);
	while (ret == 0) {

		/* General tablespaces are always at the first level of the
		data home dir */
		if (dbinfo.type == OS_FILE_TYPE_FILE) {
			bool is_isl = ends_with(dbinfo.name, ".isl");
			bool is_ibd = !is_isl && ends_with(dbinfo.name,".ibd");

			if (is_isl || is_ibd) {
				(*callback)(NULL, dbinfo.name, is_isl, false);
			}
		}

		if (dbinfo.type == OS_FILE_TYPE_FILE
		    || dbinfo.type == OS_FILE_TYPE_UNKNOWN) {

			goto next_datadir_item;
		}

		/* We found a symlink or a directory; try opening it to see
		if a symlink is a directory */

		len = strlen(fil_path_to_mysql_datadir)
			+ strlen (dbinfo.name) + 2;
		if (len > dbpath_len) {
			dbpath_len = len;

			if (dbpath) {
				ut_free(dbpath);
			}

			dbpath = static_cast<char*>(ut_malloc_nokey(dbpath_len));
		}
		snprintf(dbpath, dbpath_len,
			 "%s/%s", fil_path_to_mysql_datadir, dbinfo.name);
		os_normalize_path(dbpath);

		if (check_if_skip_database_by_path(dbpath)) {
			fprintf(stderr, "Skipping db: %s\n", dbpath);
			goto next_datadir_item;
		}

		/* We want wrong directory permissions to be a fatal error for
		XtraBackup. */
		dbdir = os_file_opendir(dbpath, true);

		if (dbdir != NULL) {

			/* We found a database directory; loop through it,
			looking for possible .ibd files in it */

			for (ret = fil_file_readdir_next_file(&err, dbpath,
							      dbdir,
							      &fileinfo);
			     ret == 0;
			     ret = fil_file_readdir_next_file(&err, dbpath,
							      dbdir,
							      &fileinfo)) {
				if (fileinfo.type == OS_FILE_TYPE_DIR) {
					continue;
				}

				/* We found a symlink or a file */
				if (strlen(fileinfo.name) > 4) {
					bool is_isl= false;
					if (ends_with(fileinfo.name, ".ibd") || ((is_isl = ends_with(fileinfo.name, ".isl"))))
						(*callback)(dbinfo.name, fileinfo.name, is_isl, false);
				}
			}

			if (0 != os_file_closedir(dbdir)) {
				fprintf(stderr, "InnoDB: Warning: could not"
				 " close database directory %s\n",
					dbpath);

				err = DB_ERROR;
			}

		} else {

			err = DB_ERROR;
			break;

		}

next_datadir_item:
		ret = fil_file_readdir_next_file(&err,
						 fil_path_to_mysql_datadir,
						 dir, &dbinfo);
	}

	ut_free(dbpath);

	if (0 != os_file_closedir(dir)) {
		fprintf(stderr,
			"InnoDB: Error: could not close MySQL datadir\n");

		return(DB_ERROR);
	}

	return(err);
}

/** Assign srv_undo_space_id_start variable if there are undo tablespace present.
Read the TRX_SYS page from ibdata1 file and get the minimum space id from
the first slot rollback segments of TRX_SYS_PAGE_NO.
@retval DB_ERROR if file open or page read failed.
@retval DB_SUCCESS if srv_undo_space_id assigned successfully. */
static dberr_t xb_assign_undo_space_start()
{

	pfs_os_file_t	file;
	byte*		buf;
	byte*		page;
	bool		ret;
	dberr_t		error = DB_SUCCESS;
	ulint		space;
	int n_retries = 5;

	if (srv_undo_tablespaces == 0) {
		return error;
	}

	file = os_file_create(0, srv_sys_space.first_datafile()->filepath(),
		OS_FILE_OPEN, OS_FILE_NORMAL, OS_DATA_FILE, true, &ret);

	if (!ret) {
		msg("Error opening %s", srv_sys_space.first_datafile()->filepath());
		return DB_ERROR;
	}

	buf = static_cast<byte*>(ut_malloc_nokey(2 * UNIV_PAGE_SIZE));
	page = static_cast<byte*>(ut_align(buf, UNIV_PAGE_SIZE));

retry:
	if (os_file_read(IORequestRead, file, page, TRX_SYS_PAGE_NO * UNIV_PAGE_SIZE,
			  UNIV_PAGE_SIZE) != DB_SUCCESS) {
		msg("Reading TRX_SYS page failed.");
		error = DB_ERROR;
		goto func_exit;
	}

	/* TRX_SYS page can't be compressed or encrypted. */
	if (buf_page_is_corrupted(false, page, univ_page_size)) {
		if (n_retries--) {
			os_thread_sleep(1000);
			goto retry;
		} else {
			msg("mariabackup: TRX_SYS page corrupted.\n");
			error = DB_ERROR;
			goto func_exit;
		}
	}

	/* 0th slot always points to system tablespace.
	1st slot should point to first undotablespace which is minimum. */

	ut_ad(mach_read_from_4(TRX_SYS + TRX_SYS_RSEGS
			       + TRX_SYS_RSEG_SLOT_SIZE
			       + TRX_SYS_RSEG_PAGE_NO + page)
	      != FIL_NULL);

	space = mach_read_ulint(TRX_SYS + TRX_SYS_RSEGS
				+ TRX_SYS_RSEG_SLOT_SIZE
				+ TRX_SYS_RSEG_SPACE + page, MLOG_4BYTES);

	srv_undo_space_id_start = space;

func_exit:
	ut_free(buf);
	ret = os_file_close(file);
	ut_a(ret);

	return error;
}

/****************************************************************************
Populates the tablespace memory cache by scanning for and opening data files.
@returns DB_SUCCESS or error code.*/
static
dberr_t
xb_load_tablespaces()
{
	bool	create_new_db;
	dberr_t	err;
	ulint   sum_of_new_sizes;
        lsn_t	flush_lsn;

	ut_ad(srv_operation == SRV_OPERATION_BACKUP
	      || srv_operation == SRV_OPERATION_RESTORE_DELTA);

	err = srv_sys_space.check_file_spec(&create_new_db, 0);

	/* create_new_db must not be true. */
	if (err != DB_SUCCESS || create_new_db) {
		msg("Could not find data files at the specified datadir");
		return(DB_ERROR);
	}

	for (int i= 0; i < 10; i++) {
		err = srv_sys_space.open_or_create(false, false, &sum_of_new_sizes,
						 &flush_lsn);
		if (err == DB_PAGE_CORRUPTED || err == DB_CORRUPTION) {
			my_sleep(1000);
		}
		else
		 break;
	}

	if (err != DB_SUCCESS) {
		msg("Could not open data files.\n");
		return(err);
	}

	/* Add separate undo tablespaces to fil_system */

	err = xb_assign_undo_space_start();

	if (err != DB_SUCCESS) {
		return err;
	}

	err = srv_undo_tablespaces_init(false);

	if (err != DB_SUCCESS) {
		return(err);
	}

	/* It is important to call xb_load_single_table_tablespaces() after
	srv_undo_tablespaces_init(), because fil_is_user_tablespace_id() *
	relies on srv_undo_tablespaces_open to be properly initialized */

	msg("mariabackup: Generating a list of tablespaces");

	err = enumerate_ibd_files(xb_load_single_table_tablespace);
	if (err != DB_SUCCESS) {
		return(err);
	}
	DBUG_MARIABACKUP_EVENT("after_load_tablespaces", 0);
	return(DB_SUCCESS);
}

/************************************************************************
Initialize the tablespace memory cache and populate it by scanning for and
opening data files.
@returns DB_SUCCESS or error code.*/
static
dberr_t
xb_data_files_init()
{
	xb_fil_io_init();

	return(xb_load_tablespaces());
}

/************************************************************************
Destroy the tablespace memory cache. */
static
void
xb_data_files_close()
{
	ut_ad(!os_thread_count);
	fil_close_all_files();
	if (buf_dblwr) {
		buf_dblwr_free();
	}
}

/***********************************************************************
Allocate and initialize the entry for databases and tables filtering
hash tables. If memory allocation is not successful, terminate program.
@return pointer to the created entry.  */
static
xb_filter_entry_t *
xb_new_filter_entry(
/*================*/
	const char*	name)	/*!< in: name of table/database */
{
	xb_filter_entry_t	*entry;
	ulint namelen = strlen(name);

	ut_a(namelen <= NAME_LEN * 2 + 1);

	entry = static_cast<xb_filter_entry_t *>
		(malloc(sizeof(xb_filter_entry_t) + namelen + 1));
	memset(entry, '\0', sizeof(xb_filter_entry_t) + namelen + 1);
	entry->name = ((char*)entry) + sizeof(xb_filter_entry_t);
	strcpy(entry->name, name);
	entry->has_tables = FALSE;

	return entry;
}

/***********************************************************************
Add entry to hash table. If hash table is NULL, allocate and initialize
new hash table */
static
xb_filter_entry_t*
xb_add_filter(
/*========================*/
	const char*	name,	/*!< in: name of table/database */
	hash_table_t**	hash)	/*!< in/out: hash to insert into */
{
	xb_filter_entry_t*	entry;

	entry = xb_new_filter_entry(name);

	if (UNIV_UNLIKELY(*hash == NULL)) {
		*hash = hash_create(1000);
	}
	HASH_INSERT(xb_filter_entry_t,
		name_hash, *hash,
		ut_fold_string(entry->name),
		entry);

	return entry;
}

/***********************************************************************
Validate name of table or database. If name is invalid, program will
be finished with error code */
static
void
xb_validate_name(
/*=============*/
	const char*	name,	/*!< in: name */
	size_t		len)	/*!< in: length of name */
{
	const char*	p;

	/* perform only basic validation. validate length and
	path symbols */
	if (len > NAME_LEN) {
		die("name `%s` is too long.", name);
	}
	p = strpbrk(name, "/\\~");
	if (p && (uint) (p - name) < NAME_LEN) {
		die("name `%s` is not valid.", name);
	}
}

/***********************************************************************
Register new filter entry which can be either database
or table name.  */
static
void
xb_register_filter_entry(
/*=====================*/
	const char*	name,	/*!< in: name */
	hash_table_t** databases_hash,
	hash_table_t** tables_hash
	)
{
	const char*		p;
	size_t			namelen;
	xb_filter_entry_t*	db_entry = NULL;

	namelen = strlen(name);
	if ((p = strchr(name, '.')) != NULL) {
		char dbname[NAME_LEN + 1];

		xb_validate_name(name, p - name);
		xb_validate_name(p + 1, namelen - (p - name));

		strncpy(dbname, name, p - name);
		dbname[p - name] = 0;

		if (*databases_hash) {
			HASH_SEARCH(name_hash, (*databases_hash),
					ut_fold_string(dbname),
					xb_filter_entry_t*,
					db_entry, (void) 0,
					!strcmp(db_entry->name, dbname));
		}
		if (!db_entry) {
			db_entry = xb_add_filter(dbname, databases_hash);
		}
		db_entry->has_tables = TRUE;
		xb_add_filter(name, tables_hash);
	} else {
		xb_validate_name(name, namelen);

		xb_add_filter(name, databases_hash);
	}
}

static
void
xb_register_include_filter_entry(
	const char* name
)
{
	xb_register_filter_entry(name, &databases_include_hash,
				 &tables_include_hash);
}

static
void
xb_register_exclude_filter_entry(
	const char* name
)
{
	xb_register_filter_entry(name, &databases_exclude_hash,
				 &tables_exclude_hash);
}

void register_ignore_db_dirs_filter(const char *name)
{
  xb_add_filter(name, &databases_exclude_hash);
}

/***********************************************************************
Register new table for the filter.  */
static
void
xb_register_table(
/*==============*/
	const char* name)	/*!< in: name of table */
{
	if (strchr(name, '.') == NULL) {
		die("`%s` is not fully qualified name.", name);
	}

	xb_register_include_filter_entry(name);
}

static
void
xb_add_regex_to_list(
	const char* regex,  /*!< in: regex */
	const char* error_context,  /*!< in: context to error message */
	regex_list_t* list) /*! in: list to put new regex to */
{
	char			errbuf[100];
	int			ret;

	regex_t compiled_regex;
	ret = regcomp(&compiled_regex, regex, REG_EXTENDED);

	if (ret != 0) {
		regerror(ret, &compiled_regex, errbuf, sizeof(errbuf));
		msg("mariabackup: error: %s regcomp(%s): %s",
			error_context, regex, errbuf);
		exit(EXIT_FAILURE);
	}

	list->push_back(compiled_regex);
}

/***********************************************************************
Register new regex for the include filter.  */
static
void
xb_register_include_regex(
/*==============*/
	const char* regex)	/*!< in: regex */
{
	xb_add_regex_to_list(regex, "tables", &regex_include_list);
}

/***********************************************************************
Register new regex for the exclude filter.  */
static
void
xb_register_exclude_regex(
/*==============*/
	const char* regex)	/*!< in: regex */
{
	xb_add_regex_to_list(regex, "tables-exclude", &regex_exclude_list);
}

typedef void (*insert_entry_func_t)(const char*);

/* Scan string and load filter entries from it.
@param[in] list string representing a list
@param[in] delimiters delimiters of entries
@param[in] ins callback to add entry */
void xb_load_list_string(char *list, const char *delimiters,
                         insert_entry_func_t ins)
{
  char *p;
  char *saveptr;

  p= strtok_r(list, delimiters, &saveptr);
  while (p)
  {

    ins(p);

    p= strtok_r(NULL, delimiters, &saveptr);
  }
}

/***********************************************************************
Scan file and load filter entries from it.  */
static
void
xb_load_list_file(
/*==============*/
	const char* filename,		/*!< in: name of file */
	insert_entry_func_t ins)	/*!< in: callback to add entry */
{
	char	name_buf[NAME_LEN*2+2];
	FILE*	fp;

	/* read and store the filenames */
	fp = fopen(filename, "r");
	if (!fp) {
		die("Can't open %s",
		    filename);
	}
	while (fgets(name_buf, sizeof(name_buf), fp) != NULL) {
		char*	p = strchr(name_buf, '\n');
		if (p) {
			*p = '\0';
		} else {
			die("`%s...` name is too long", name_buf);
		}

		ins(name_buf);
	}

	fclose(fp);
}


static
void
xb_filters_init()
{
	if (xtrabackup_databases) {
		xb_load_list_string(xtrabackup_databases, " \t",
				    xb_register_include_filter_entry);
	}

	if (xtrabackup_databases_file) {
		xb_load_list_file(xtrabackup_databases_file,
				  xb_register_include_filter_entry);
	}

	if (xtrabackup_databases_exclude) {
		xb_load_list_string(xtrabackup_databases_exclude, " \t",
				    xb_register_exclude_filter_entry);
	}

	if (xtrabackup_tables) {
		xb_load_list_string(xtrabackup_tables, ",",
				    xb_register_include_regex);
	}

	if (xtrabackup_tables_file) {
		xb_load_list_file(xtrabackup_tables_file, xb_register_table);
	}

	if (xtrabackup_tables_exclude) {
		xb_load_list_string(xtrabackup_tables_exclude, ",",
				    xb_register_exclude_regex);
	}
}

static
void
xb_filter_hash_free(hash_table_t* hash)
{
	ulint	i;

	/* free the hash elements */
	for (i = 0; i < hash_get_n_cells(hash); i++) {
		xb_filter_entry_t*	table;

		table = static_cast<xb_filter_entry_t *>
			(HASH_GET_FIRST(hash, i));

		while (table) {
			xb_filter_entry_t*	prev_table = table;

			table = static_cast<xb_filter_entry_t *>
				(HASH_GET_NEXT(name_hash, prev_table));

			HASH_DELETE(xb_filter_entry_t, name_hash, hash,
				ut_fold_string(prev_table->name), prev_table);
			free(prev_table);
		}
	}

	/* free hash */
	hash_table_free(hash);
}

static void xb_regex_list_free(regex_list_t* list)
{
	while (list->size() > 0) {
		xb_regfree(&list->front());
		list->pop_front();
	}
}

/************************************************************************
Destroy table filters for partial backup. */
static
void
xb_filters_free()
{
	xb_regex_list_free(&regex_include_list);
	xb_regex_list_free(&regex_exclude_list);

	if (tables_include_hash) {
		xb_filter_hash_free(tables_include_hash);
	}

	if (tables_exclude_hash) {
		xb_filter_hash_free(tables_exclude_hash);
	}

	if (databases_include_hash) {
		xb_filter_hash_free(databases_include_hash);
	}

	if (databases_exclude_hash) {
		xb_filter_hash_free(databases_exclude_hash);
	}
}

/*********************************************************************//**
Create log file metadata. */
static
void
open_or_create_log_file(
/*====================*/
	fil_space_t* space,
	ulint	i)			/*!< in: log file number in group */
{
	char	name[FN_REFLEN];
	ulint	dirnamelen;

	os_normalize_path(srv_log_group_home_dir);

	dirnamelen = strlen(srv_log_group_home_dir);
	ut_a(dirnamelen < (sizeof name) - 10 - sizeof "ib_logfile");
	memcpy(name, srv_log_group_home_dir, dirnamelen);

	/* Add a path separator if needed. */
	if (dirnamelen && name[dirnamelen - 1] != OS_PATH_SEPARATOR) {
		name[dirnamelen++] = OS_PATH_SEPARATOR;
	}

	sprintf(name + dirnamelen, "%s%zu", "ib_logfile", i);

	ut_a(fil_validate());

	space->add(name, OS_FILE_CLOSED,
		   ulint(srv_log_file_size >> srv_page_size_shift),
		   false, false);
}

/*********************************************************************//**
Normalizes init parameter values to use units we use inside InnoDB.
@return	DB_SUCCESS or error code */
static
void
xb_normalize_init_values(void)
/*==========================*/
{
	srv_sys_space.normalize();
	srv_log_buffer_size /= UNIV_PAGE_SIZE;
	srv_lock_table_size = 5 * (srv_buf_pool_size / UNIV_PAGE_SIZE);
}

/***********************************************************************
Set the open files limit. Based on set_max_open_files().

@return the resulting open files limit. May be less or more than the requested
value.  */
static uint
xb_set_max_open_files(
/*==================*/
	uint max_file_limit)	/*!<in: open files limit */
{
#if defined(RLIMIT_NOFILE)
	struct rlimit rlimit;
	uint old_cur;

	if (getrlimit(RLIMIT_NOFILE, &rlimit)) {

		goto end;
	}

	old_cur = (uint) rlimit.rlim_cur;

	if (rlimit.rlim_cur == RLIM_INFINITY) {

		rlimit.rlim_cur = max_file_limit;
	}

	if (rlimit.rlim_cur >= max_file_limit) {

		max_file_limit = rlimit.rlim_cur;
		goto end;
	}

	rlimit.rlim_cur = rlimit.rlim_max = max_file_limit;

	if (setrlimit(RLIMIT_NOFILE, &rlimit)) {

		max_file_limit = old_cur;	/* Use original value */
	} else {

		rlimit.rlim_cur = 0;	/* Safety if next call fails */

		(void) getrlimit(RLIMIT_NOFILE, &rlimit);

		if (rlimit.rlim_cur) {

			/* If call didn't fail */
			max_file_limit = (uint) rlimit.rlim_cur;
		}
	}

end:
	return(max_file_limit);
#else
	return(0);
#endif
}

static void stop_backup_threads()
{
	if (log_copying_stop && log_copying_running) {
		os_event_set(log_copying_stop);
		fputs("mariabackup: Stopping log copying thread", stderr);
		fflush(stderr);
		while (log_copying_running) {
			putc('.', stderr);
			fflush(stderr);
			os_thread_sleep(200000); /*0.2 sec*/
		}
		putc('\n', stderr);
		os_event_destroy(log_copying_stop);
	}

	if (wait_throttle) {
		/* wait for io_watching_thread completion */
		while (io_watching_thread_running) {
			os_thread_sleep(1000000);
		}
		os_event_destroy(wait_throttle);
	}
}

/** Implement the core of --backup
@return	whether the operation succeeded */
static bool xtrabackup_backup_low()
{
	ut_ad(!metadata_to_lsn);

	/* read the latest checkpoint lsn */
	{
		ulint	max_cp_field;

		log_mutex_enter();

		if (recv_find_max_checkpoint(&max_cp_field) == DB_SUCCESS
		    && log_sys->log.format != 0) {
			if (max_cp_field == LOG_CHECKPOINT_1) {
				log_group_header_read(&log_sys->log,
						      max_cp_field);
			}
			metadata_to_lsn = mach_read_from_8(
				log_sys->checkpoint_buf + LOG_CHECKPOINT_LSN);
			msg("The latest check point"
			    " (for incremental): '" LSN_PF "'",
			    metadata_to_lsn);
		} else {
			msg("Error: recv_find_max_checkpoint() failed.");
		}
		log_mutex_exit();
	}

	stop_backup_threads();

	if (metadata_to_lsn && xtrabackup_copy_logfile(true)) {
		ds_close(dst_log_file);
		dst_log_file = NULL;
		return false;
	}

	if (ds_close(dst_log_file) || !metadata_to_lsn) {
		dst_log_file = NULL;
		return false;
	}

	dst_log_file = NULL;

	if(!xtrabackup_incremental) {
		strcpy(metadata_type, "full-backuped");
		metadata_from_lsn = 0;
	} else {
		strcpy(metadata_type, "incremental");
		metadata_from_lsn = incremental_lsn;
	}
	metadata_last_lsn = log_copy_scanned_lsn;

	if (!xtrabackup_stream_metadata(ds_meta)) {
		msg("Error: failed to stream metadata.");
		return false;
	}
	if (xtrabackup_extra_lsndir) {
		char	filename[FN_REFLEN];

		sprintf(filename, "%s/%s", xtrabackup_extra_lsndir,
			XTRABACKUP_METADATA_FILENAME);
		if (!xtrabackup_write_metadata(filename)) {
			msg("Error: failed to write metadata "
			    "to '%s'.", filename);
			return false;
		}
		sprintf(filename, "%s/%s", xtrabackup_extra_lsndir,
			XTRABACKUP_INFO);
		if (!write_xtrabackup_info(mysql_connection, filename, false, false)) {
			msg("Error: failed to write info "
			 "to '%s'.", filename);
			return false;
		}
	}

	return true;
}

/** Implement --backup
@return	whether the operation succeeded */
static bool xtrabackup_backup_func()
{
	MY_STAT			 stat_info;
	uint			 i;
	uint			 count;
	pthread_mutex_t		 count_mutex;
	CorruptedPages corrupted_pages;
	data_thread_ctxt_t 	*data_threads;
	pthread_mutex_init(&backup_mutex, NULL);
	pthread_cond_init(&scanned_lsn_cond, NULL);

#ifdef USE_POSIX_FADVISE
	msg("uses posix_fadvise().");
#endif

	/* cd to datadir */

	if (my_setwd(mysql_real_data_home,MYF(MY_WME)))
	{
		msg("my_setwd() failed , %s", mysql_real_data_home);
		return(false);
	}
	msg("cd to %s", mysql_real_data_home);
	encryption_plugin_backup_init(mysql_connection);
	msg("open files limit requested %u, set to %u",
	    (uint) xb_open_files_limit,
	    xb_set_max_open_files(xb_open_files_limit));

	mysql_data_home= mysql_data_home_buff;
	mysql_data_home[0]=FN_CURLIB;		// all paths are relative from here
	mysql_data_home[1]=0;

	srv_n_purge_threads = 1;
	srv_read_only_mode = TRUE;

	srv_operation = SRV_OPERATION_BACKUP;
	log_file_op = backup_file_op;
	metadata_to_lsn = 0;

	/* initialize components */
        if(innodb_init_param()) {
fail:
		metadata_to_lsn = log_copying_running;
		stop_backup_threads();
		log_file_op = NULL;
		if (dst_log_file) {
			ds_close(dst_log_file);
			dst_log_file = NULL;
		}
		if (fil_system) {
			innodb_shutdown();
		}
		return(false);
	}

	xb_normalize_init_values();

	if (srv_file_flush_method_str == NULL) {
		/* These are the default options */
		srv_file_flush_method = SRV_FSYNC;
	} else if (0 == ut_strcmp(srv_file_flush_method_str, "fsync")) {
		srv_file_flush_method = SRV_FSYNC;
	} else if (0 == ut_strcmp(srv_file_flush_method_str, "O_DSYNC")) {
		srv_file_flush_method = SRV_O_DSYNC;

	} else if (0 == ut_strcmp(srv_file_flush_method_str, "O_DIRECT")) {
		srv_file_flush_method = SRV_O_DIRECT;
		msg("using O_DIRECT");
	} else if (0 == ut_strcmp(srv_file_flush_method_str, "littlesync")) {
		srv_file_flush_method = SRV_LITTLESYNC;
	} else if (0 == ut_strcmp(srv_file_flush_method_str, "nosync")) {
		srv_file_flush_method = SRV_NOSYNC;
	} else if (0 == ut_strcmp(srv_file_flush_method_str, "ALL_O_DIRECT")) {
		srv_file_flush_method = SRV_ALL_O_DIRECT_FSYNC;
		msg("using ALL_O_DIRECT");
	} else if (0 == ut_strcmp(srv_file_flush_method_str,
				  "O_DIRECT_NO_FSYNC")) {
		srv_file_flush_method = SRV_O_DIRECT_NO_FSYNC;
		msg("using O_DIRECT_NO_FSYNC");
	} else {
		msg("Unrecognized value %s for "
		    "innodb_flush_method", srv_file_flush_method_str);
		goto fail;
	}

#ifdef _WIN32
  srv_file_flush_method = SRV_ALL_O_DIRECT_FSYNC;
	srv_use_native_aio = TRUE;
#endif

	if (srv_buf_pool_size >= 1000 * 1024 * 1024) {
                                  /* Here we still have srv_pool_size counted
                                  in kilobytes (in 4.0 this was in bytes)
				  srv_boot() converts the value to
                                  pages; if buffer pool is less than 1000 MB,
                                  assume fewer threads. */
                srv_max_n_threads = 50000;

	} else if (srv_buf_pool_size >= 8 * 1024 * 1024) {

                srv_max_n_threads = 10000;
        } else {
		srv_max_n_threads = 1000;       /* saves several MB of memory,
                                                especially in 64-bit
                                                computers */
        }

	sync_check_init();
	ut_d(sync_check_enable());
	/* Reset the system variables in the recovery module. */
	recv_sys_var_init();
	trx_pool_init();

	ut_crc32_init();
	crc_init();
	recv_sys_init();

#ifdef WITH_INNODB_DISALLOW_WRITES
	srv_allow_writes_event = os_event_create(0);
	os_event_set(srv_allow_writes_event);
#endif

	xb_filters_init();

	xb_fil_io_init();
	srv_n_file_io_threads = srv_n_read_io_threads;

	os_aio_init(srv_n_read_io_threads, srv_n_write_io_threads,
		    SRV_MAX_N_PENDING_SYNC_IOS);

	log_sys_init();
	log_init(srv_n_log_files);
	fil_space_t*	space = fil_space_create(
		"innodb_redo_log", SRV_LOG_SPACE_FIRST_ID, 0,
		FIL_TYPE_LOG, NULL);

	lock_sys_create(srv_lock_table_size);

	for (ulint i = 0; i < srv_n_log_files; i++) {
		open_or_create_log_file(space, i);
	}

	/* create extra LSN dir if it does not exist. */
	if (xtrabackup_extra_lsndir
		&&!my_stat(xtrabackup_extra_lsndir,&stat_info,MYF(0))
		&& (my_mkdir(xtrabackup_extra_lsndir,0777,MYF(0)) < 0)) {
		msg("Error: cannot mkdir %d: %s\n",
		    my_errno, xtrabackup_extra_lsndir);
		goto fail;
	}

	/* create target dir if not exist */
	if (!xtrabackup_stream_str && !my_stat(xtrabackup_target_dir,&stat_info,MYF(0))
		&& (my_mkdir(xtrabackup_target_dir,0777,MYF(0)) < 0)){
		msg("Error: cannot mkdir %d: %s\n",
		    my_errno, xtrabackup_target_dir);
		goto fail;
	}

        {
	/* definition from recv_recovery_from_checkpoint_start() */
	ulint		max_cp_field;

	/* start back ground thread to copy newer log */
	os_thread_id_t log_copying_thread_id;

	/* get current checkpoint_lsn */
	/* Look for the latest checkpoint from any of the log groups */

	log_mutex_enter();

reread_log_header:
	dberr_t err = recv_find_max_checkpoint(&max_cp_field);

	if (err != DB_SUCCESS) {
		msg("Error: cannot read redo log header");
		log_mutex_exit();
		goto fail;
	}

	if (log_sys->log.format == 0) {
		msg("Error: cannot process redo log before MariaDB 10.2.2");
		log_mutex_exit();
		goto fail;
	}

	const byte* buf = log_sys->checkpoint_buf;
	checkpoint_lsn_start = log_sys->log.lsn;
	checkpoint_no_start = log_sys->next_checkpoint_no;

	log_group_header_read(&log_sys->log, max_cp_field);

	if (checkpoint_no_start != mach_read_from_8(buf + LOG_CHECKPOINT_NO)
		|| checkpoint_lsn_start != mach_read_from_8(buf + LOG_CHECKPOINT_LSN)
		|| log_sys->log.lsn_offset
			!= mach_read_from_8(buf + LOG_CHECKPOINT_OFFSET))
		goto reread_log_header;

	log_mutex_exit();

	xtrabackup_init_datasinks();

	if (!select_history()) {
		goto fail;
	}

	/* open the log file */
	memset(&stat_info, 0, sizeof(MY_STAT));
	dst_log_file = ds_open(ds_redo, "ib_logfile0", &stat_info);
	if (dst_log_file == NULL) {
		msg("Error: failed to open the target stream for "
		    "'ib_logfile0'.");
		goto fail;
	}

	/* label it */
	byte MY_ALIGNED(OS_FILE_LOG_BLOCK_SIZE) log_hdr_buf[LOG_FILE_HDR_SIZE];
	memset(log_hdr_buf, 0, sizeof log_hdr_buf);

	byte *log_hdr_field = log_hdr_buf;
	mach_write_to_4(LOG_HEADER_FORMAT + log_hdr_field, log_sys->log.format);
	mach_write_to_4(LOG_HEADER_SUBFORMAT + log_hdr_field, log_sys->log.subformat);
	mach_write_to_8(LOG_HEADER_START_LSN + log_hdr_field, checkpoint_lsn_start);
	strcpy(reinterpret_cast<char*>(LOG_HEADER_CREATOR + log_hdr_field),
		"Backup " MYSQL_SERVER_VERSION);
	log_block_set_checksum(log_hdr_field,
		log_block_calc_checksum_crc32(log_hdr_field));

	/* copied from log_group_checkpoint() */
	log_hdr_field +=
		(log_sys->next_checkpoint_no & 1) ? LOG_CHECKPOINT_2 : LOG_CHECKPOINT_1;
	/* The least significant bits of LOG_CHECKPOINT_OFFSET must be
	stored correctly in the copy of the ib_logfile. The most significant
	bits, which identify the start offset of the log block in the file,
	we did choose freely, as LOG_FILE_HDR_SIZE. */
	ut_ad(!((log_sys->log.lsn ^ checkpoint_lsn_start)
		& (OS_FILE_LOG_BLOCK_SIZE - 1)));
	/* Adjust the checkpoint page. */
	memcpy(log_hdr_field, log_sys->checkpoint_buf, OS_FILE_LOG_BLOCK_SIZE);
	mach_write_to_8(log_hdr_field + LOG_CHECKPOINT_OFFSET,
		(checkpoint_lsn_start & (OS_FILE_LOG_BLOCK_SIZE - 1))
		| LOG_FILE_HDR_SIZE);
	log_block_set_checksum(log_hdr_field,
			log_block_calc_checksum_crc32(log_hdr_field));

	/* Write log header*/
	if (ds_write(dst_log_file, log_hdr_buf, sizeof(log_hdr_buf))) {
		msg("error: write to logfile failed");
		goto fail;
	}

	log_copying_running = true;
	/* start io throttle */
	if(xtrabackup_throttle) {
		os_thread_id_t io_watching_thread_id;

		io_ticket = xtrabackup_throttle;
		wait_throttle = os_event_create(0);
		io_watching_thread_running = true;

		os_thread_create(io_watching_thread, NULL,
				 &io_watching_thread_id);
	}

	/* Populate fil_system with tablespaces to copy */
	err = xb_load_tablespaces();
	if (err != DB_SUCCESS) {
		msg("merror: xb_load_tablespaces() failed with"
		    " error %s.", ut_strerr(err));
fail_before_log_copying_thread_start:
		log_copying_running = false;
		goto fail;
	}

	/* copy log file by current position */
	log_copy_scanned_lsn = checkpoint_lsn_start;
	recv_sys->recovered_lsn = log_copy_scanned_lsn;
	log_optimized_ddl_op = backup_optimized_ddl_op;
	log_truncate = backup_truncate_fail;

	if (xtrabackup_copy_logfile())
		goto fail_before_log_copying_thread_start;

	DBUG_MARIABACKUP_EVENT("before_innodb_log_copy_thread_started",0);

	log_copying_stop = os_event_create(0);
	os_thread_create(log_copying_thread, NULL, &log_copying_thread_id);

	/* FLUSH CHANGED_PAGE_BITMAPS call */
	if (!flush_changed_page_bitmaps()) {
		goto fail;
	}

	ut_a(xtrabackup_parallel > 0);

	if (xtrabackup_parallel > 1) {
		msg("mariabackup: Starting %u threads for parallel data "
		    "files transfer", xtrabackup_parallel);
	}

	if (opt_lock_ddl_per_table) {
		mdl_lock_all();

		DBUG_EXECUTE_IF("check_mdl_lock_works",
			dbug_alter_thread_done =
			dbug_start_query_thread("ALTER TABLE test.t ADD COLUMN mdl_lock_column int",
				"Waiting for table metadata lock", 1, ER_QUERY_INTERRUPTED););
	}

	datafiles_iter_t *it = datafiles_iter_new(fil_system);
	if (it == NULL) {
		msg("mariabackup: Error: datafiles_iter_new() failed.");
		goto fail;
	}

	/* Create data copying threads */
	data_threads = (data_thread_ctxt_t *)
		malloc(sizeof(data_thread_ctxt_t) * xtrabackup_parallel);
	count = xtrabackup_parallel;
	pthread_mutex_init(&count_mutex, NULL);

	for (i = 0; i < (uint) xtrabackup_parallel; i++) {
		data_threads[i].it = it;
		data_threads[i].num = i+1;
		data_threads[i].count = &count;
		data_threads[i].count_mutex = &count_mutex;
		data_threads[i].corrupted_pages = &corrupted_pages;
		os_thread_create(data_copy_thread_func, data_threads + i,
				 &data_threads[i].id);
	}

	/* Wait for threads to exit */
	while (1) {
		os_thread_sleep(1000000);
		pthread_mutex_lock(&count_mutex);
		bool stop = count == 0;
		pthread_mutex_unlock(&count_mutex);
		if (stop) {
			break;
		}
	}

	pthread_mutex_destroy(&count_mutex);
	free(data_threads);
	datafiles_iter_free(it);
	}

	bool ok = backup_start(corrupted_pages);

	if (ok) {
		ok = xtrabackup_backup_low();

		backup_release();

		DBUG_EXECUTE_IF("check_mdl_lock_works",
			os_event_wait(dbug_alter_thread_done);
			os_event_destroy(dbug_alter_thread_done);
		);

		if (ok) {
			backup_finish();
		}
	}

	if (opt_log_innodb_page_corruption)
		ok = corrupted_pages.print_to_file(MB_CORRUPTED_PAGES_FILE);

	if (!ok) {
		goto fail;
	}

	if (changed_page_bitmap) {
		xb_page_bitmap_deinit(changed_page_bitmap);
	}
	xtrabackup_destroy_datasinks();

	msg("Redo log (from LSN " LSN_PF " to " LSN_PF
	    ") was copied.", checkpoint_lsn_start, log_copy_scanned_lsn);
	xb_filters_free();

	xb_data_files_close();

	/* Make sure that the latest checkpoint was included */
	if (metadata_to_lsn > log_copy_scanned_lsn) {
		msg("Error: failed to copy enough redo log ("
		    "LSN=" LSN_PF "; checkpoint LSN=" LSN_PF ").",
		    log_copy_scanned_lsn, metadata_to_lsn);
		goto fail;
	}

	innodb_shutdown();
	log_file_op = NULL;
	pthread_mutex_destroy(&backup_mutex);
	pthread_cond_destroy(&scanned_lsn_cond);
	if (!corrupted_pages.empty()) {
		ut_ad(opt_log_innodb_page_corruption);
		msg("Error: corrupted innodb pages are found and logged to "
			MB_CORRUPTED_PAGES_FILE " file");
	}
	return(true);
}


/**
This function handles DDL changes at the end of backup, under protection of
FTWRL.  This ensures consistent backup in presence of DDL.

- New tables, that were created during backup, are now copied into backup.
  Also, tablespaces with optimized (no redo loggin DDL) are re-copied into 
  backup. This tablespaces will get the extension ".new" in the backup

- Tables that were renamed during backup, are marked as renamed
  For these, file <old_name>.ren will be created.
  The content of the file is the new tablespace name.

- Tables that were deleted during backup, are marked as deleted
  For these , an empty file <name>.del will be created

  It is the responsibility of the prepare phase to deal with .new, .ren, and .del
  files.
*/
void backup_fix_ddl(CorruptedPages &corrupted_pages)
{
	std::set<std::string> new_tables;
	std::set<std::string> dropped_tables;
	std::map<std::string, std::string> renamed_tables;

	/* Disable further DDL on backed up tables (only needed for --no-lock).*/
	pthread_mutex_lock(&backup_mutex);
	log_file_op = backup_file_op_fail;
	log_optimized_ddl_op = backup_optimized_ddl_op_fail;
	pthread_mutex_unlock(&backup_mutex);

	DBUG_MARIABACKUP_EVENT("backup_fix_ddl",0);

	for (space_id_to_name_t::iterator iter = ddl_tracker.tables_in_backup.begin();
		iter != ddl_tracker.tables_in_backup.end();
		iter++) {

		const std::string name = iter->second;
		ulint id = iter->first;

		if (ddl_tracker.drops.find(id) != ddl_tracker.drops.end()) {
			dropped_tables.insert(name);
			corrupted_pages.drop_space(id);
			continue;
		}

		bool has_optimized_ddl =
			ddl_tracker.optimized_ddl.find(id) != ddl_tracker.optimized_ddl.end();

		if (ddl_tracker.id_to_name.find(id) == ddl_tracker.id_to_name.end()) {
			if (has_optimized_ddl) {
				new_tables.insert(name);
			}
			continue;
		}

		/* tablespace was affected by DDL. */
		const std::string new_name = ddl_tracker.id_to_name[id];
		if (new_name != name) {
			if (has_optimized_ddl) {
				/* table was renamed, but we need a full copy
				of it because of optimized DDL. We emulate a drop/create.*/
				dropped_tables.insert(name);
				if (opt_log_innodb_page_corruption)
					corrupted_pages.drop_space(id);
				new_tables.insert(new_name);
			} else {
				/* Renamed, and no optimized DDL*/
				renamed_tables[name] = new_name;
				if (opt_log_innodb_page_corruption)
					corrupted_pages.rename_space(id, new_name);
			}
		} else if (has_optimized_ddl) {
			/* Table was recreated, or optimized DDL ran.
			In both cases we need a full copy in the backup.*/
			new_tables.insert(name);
			if (opt_log_innodb_page_corruption)
				corrupted_pages.drop_space(id);
		}
	}

	/* Find tables that were created during backup (and not removed).*/
	for(space_id_to_name_t::iterator iter = ddl_tracker.id_to_name.begin();
		iter != ddl_tracker.id_to_name.end();
		iter++) {

		ulint id = iter->first;
		std::string name = iter->second;

		if (ddl_tracker.tables_in_backup.find(id) != ddl_tracker.tables_in_backup.end()) {
			/* already processed above */
			continue;
		}

		if (ddl_tracker.drops.find(id) == ddl_tracker.drops.end()) {
			dropped_tables.erase(name);
			new_tables.insert(name);
			if (opt_log_innodb_page_corruption)
				corrupted_pages.drop_space(id);
		}
	}

	// Mark tablespaces for rename
	for (std::map<std::string, std::string>::iterator iter = renamed_tables.begin();
		iter != renamed_tables.end(); ++iter) {
		const std::string old_name = iter->first;
		std::string new_name = iter->second;
		backup_file_printf((old_name + ".ren").c_str(), "%s", new_name.c_str());
	}

	// Mark tablespaces for drop
	for (std::set<std::string>::iterator iter = dropped_tables.begin();
		iter != dropped_tables.end();
		iter++) {
		const std::string name(*iter);
		backup_file_printf((name + ".del").c_str(), "%s", "");
	}

	//  Load and copy new tables.
	//  Close all datanodes first, reload only new tables.
	std::vector<fil_node_t *> all_nodes;
	datafiles_iter_t *it = datafiles_iter_new(fil_system);
	if (!it)
		return;
	while (fil_node_t *node = datafiles_iter_next(it)) {
		all_nodes.push_back(node);
	}
	for (size_t i = 0; i < all_nodes.size(); i++) {
		fil_node_t *n = all_nodes[i];
		if (n->space->id == 0)
			continue;
		fil_space_close(n->space->name);
		fil_space_free(n->space->id, false);
	}
	datafiles_iter_free(it);

	for (std::set<std::string>::iterator iter = new_tables.begin();
		iter != new_tables.end(); iter++) {
		const char *space_name = iter->c_str();
		if (check_if_skip_table(space_name))
			continue;
		xb_load_single_table_tablespace(*iter, false);
	}

	it = datafiles_iter_new(fil_system);
	if (!it)
		return;

	while (fil_node_t *node = datafiles_iter_next(it)) {
		fil_space_t * space = node->space;
		if (!fil_is_user_tablespace_id(space->id))
			continue;
		std::string dest_name(node->space->name);
		dest_name.append(".new");
		xtrabackup_copy_datafile(node, 0, dest_name.c_str(), wf_write_through,
			corrupted_pages);
	}

	datafiles_iter_free(it);
}

/* ================= prepare ================= */

/***********************************************************************
Generates path to the meta file path from a given path to an incremental .delta
by replacing trailing ".delta" with ".meta", or returns error if 'delta_path'
does not end with the ".delta" character sequence.
@return TRUE on success, FALSE on error. */
static
ibool
get_meta_path(
	const char	*delta_path,	/* in: path to a .delta file */
	char 		*meta_path)	/* out: path to the corresponding .meta
					file */
{
	size_t		len = strlen(delta_path);

	if (len <= 6 || strcmp(delta_path + len - 6, ".delta")) {
		return FALSE;
	}
	memcpy(meta_path, delta_path, len - 6);
	strcpy(meta_path + len - 6, XB_DELTA_INFO_SUFFIX);

	return TRUE;
}

/****************************************************************//**
Create a new tablespace on disk and return the handle to its opened
file. Code adopted from fil_create_new_single_table_tablespace with
the main difference that only disk file is created without updating
the InnoDB in-memory dictionary data structures.

@return true on success, false on error.  */
static
bool
xb_space_create_file(
/*==================*/
	const char*	path,		/*!<in: path to tablespace */
	ulint		space_id,	/*!<in: space id */
	ulint		flags,		/*!<in: tablespace flags */
	pfs_os_file_t*	file)		/*!<out: file handle */
{
	bool		ret;
	byte*		buf;
	byte*		page;

	*file = os_file_create_simple_no_error_handling(
		0, path, OS_FILE_CREATE, OS_FILE_READ_WRITE, false, &ret);
	if (!ret) {
		msg("Can't create file %s", path);
		return ret;
	}

	ret = os_file_set_size(path, *file,
			       FIL_IBD_FILE_INITIAL_SIZE * UNIV_PAGE_SIZE);
	if (!ret) {
		msg("mariabackup: cannot set size for file %s", path);
		os_file_close(*file);
		os_file_delete(0, path);
		return ret;
	}

	buf = static_cast<byte *>(malloc(3 * UNIV_PAGE_SIZE));
	/* Align the memory for file i/o if we might have O_DIRECT set */
	page = static_cast<byte *>(ut_align(buf, UNIV_PAGE_SIZE));

	memset(page, '\0', UNIV_PAGE_SIZE);

	fsp_header_init_fields(page, space_id, flags);
	mach_write_to_4(page + FIL_PAGE_ARCH_LOG_NO_OR_SPACE_ID, space_id);

	const page_size_t page_size(flags);

	if (!page_size.is_compressed()) {
		buf_flush_init_for_writing(NULL, page, NULL, 0);

		ret = os_file_write(IORequestWrite, path, *file, page, 0,
				    UNIV_PAGE_SIZE);
	} else {
		page_zip_des_t	page_zip;
		ulint zip_size = page_size.physical();
		page_zip_set_size(&page_zip, zip_size);
		page_zip.data = page + UNIV_PAGE_SIZE;
		fprintf(stderr, "zip_size = " ULINTPF "\n", zip_size);

#ifdef UNIV_DEBUG
		page_zip.m_start =
#endif /* UNIV_DEBUG */
			page_zip.m_end = page_zip.m_nonempty =
			page_zip.n_blobs = 0;

		buf_flush_init_for_writing(NULL, page, &page_zip, 0);

		ret = os_file_write(IORequestWrite, path, *file,
				    page_zip.data, 0, zip_size);
	}

	free(buf);

	if (ret != DB_SUCCESS) {
		msg("mariabackup: could not write the first page to %s",
		    path);
		os_file_close(*file);
		os_file_delete(0, path);
		return ret;
	}

	return TRUE;
}

/***********************************************************************
Searches for matching tablespace file for given .delta file and space_id
in given directory. When matching tablespace found, renames it to match the
name of .delta file. If there was a tablespace with matching name and
mismatching ID, renames it to xtrabackup_tmp_#ID.ibd. If there was no
matching file, creates a new tablespace.
@return file handle of matched or created file */
static
pfs_os_file_t
xb_delta_open_matching_space(
	const char*	dbname,		/* in: path to destination database dir */
	const char*	name,		/* in: name of delta file (without .delta) */
	const xb_delta_info_t& info,
	char*		real_name,	/* out: full path of destination file */
	size_t		real_name_len,	/* out: buffer size for real_name */
	bool* 		success)	/* out: indicates error. true = success */
{
	char			dest_dir[FN_REFLEN];
	char			dest_space_name[FN_REFLEN];
	fil_space_t*		fil_space;
	pfs_os_file_t		file;
	xb_filter_entry_t*	table;

	ut_a(dbname != NULL ||
	     !fil_is_user_tablespace_id(info.space_id) ||
	     info.space_id == ULINT_UNDEFINED);

	*success = false;

	if (dbname) {
		snprintf(dest_dir, FN_REFLEN, "%s/%s",
			xtrabackup_target_dir, dbname);
		os_normalize_path(dest_dir);

		snprintf(dest_space_name, FN_REFLEN, "%s/%s", dbname, name);
	} else {
		snprintf(dest_dir, FN_REFLEN, "%s", xtrabackup_target_dir);
		os_normalize_path(dest_dir);

		snprintf(dest_space_name, FN_REFLEN, "%s", name);
	}

	snprintf(real_name, real_name_len,
		 "%s/%s",
		 xtrabackup_target_dir, dest_space_name);
	os_normalize_path(real_name);
	/* Truncate ".ibd" */
	dest_space_name[strlen(dest_space_name) - 4] = '\0';

	/* Create the database directory if it doesn't exist yet */
	if (!os_file_create_directory(dest_dir, FALSE)) {
		msg("mariabackup: error: cannot create dir %s", dest_dir);
		return file;
	}

	log_mutex_enter();
	if (!fil_is_user_tablespace_id(info.space_id)) {
found:
		/* open the file and return its handle */

		file = os_file_create_simple_no_error_handling(
			0, real_name,
			OS_FILE_OPEN, OS_FILE_READ_WRITE, false, success);

		if (!*success) {
			msg("mariabackup: Cannot open file %s\n", real_name);
		}
exit:
		log_mutex_exit();
		return file;
	}

	/* remember space name for further reference */
	table = static_cast<xb_filter_entry_t *>
		(malloc(sizeof(xb_filter_entry_t) +
			strlen(dest_space_name) + 1));

	table->name = ((char*)table) + sizeof(xb_filter_entry_t);
	strcpy(table->name, dest_space_name);
	HASH_INSERT(xb_filter_entry_t, name_hash, inc_dir_tables_hash,
			ut_fold_string(table->name), table);

	mutex_enter(&fil_system->mutex);
	fil_space = fil_space_get_by_name(dest_space_name);
	mutex_exit(&fil_system->mutex);

	if (fil_space != NULL) {
		if (fil_space->id == info.space_id
		    || info.space_id == ULINT_UNDEFINED) {
			/* we found matching space */
			goto found;
		} else {

			char	tmpname[FN_REFLEN];

			snprintf(tmpname, FN_REFLEN, "%s/xtrabackup_tmp_#" ULINTPF,
				 dbname, fil_space->id);

			msg("mariabackup: Renaming %s to %s.ibd",
				fil_space->name, tmpname);

			if (!fil_rename_tablespace(
				fil_space->id,
				fil_space->chain.start->name,
				tmpname, NULL))
			{
				msg("mariabackup: Cannot rename %s to %s",
					fil_space->name, tmpname);
				goto exit;
			}
		}
	}

	if (info.space_id == ULINT_UNDEFINED)
	{
		die("Can't handle DDL operation on tablespace "
		    "%s\n", dest_space_name);
	}
	mutex_enter(&fil_system->mutex);
	fil_space = fil_space_get_by_id(info.space_id);
	mutex_exit(&fil_system->mutex);
	if (fil_space != NULL) {
		char	tmpname[FN_REFLEN];

		strncpy(tmpname, dest_space_name, FN_REFLEN);

		msg("mariabackup: Renaming %s to %s",
		    fil_space->name, dest_space_name);

		if (!fil_rename_tablespace(fil_space->id,
					   fil_space->chain.start->name,
					   tmpname,
					   NULL))
		{
			msg("mariabackup: Cannot rename %s to %s",
				fil_space->name, dest_space_name);
			goto exit;
		}

		goto found;
	}

	/* No matching space found. create the new one.  */
	const ulint flags = info.page_size.is_compressed()
		? get_bit_shift(info.page_size.physical()
				>> (UNIV_ZIP_SIZE_SHIFT_MIN - 1))
		<< FSP_FLAGS_POS_ZIP_SSIZE
		| FSP_FLAGS_MASK_POST_ANTELOPE
		| FSP_FLAGS_MASK_ATOMIC_BLOBS
		| (info.page_size.logical() == UNIV_PAGE_SIZE_ORIG
		   ? 0
		   : get_bit_shift(info.page_size.logical()
				   >> (UNIV_ZIP_SIZE_SHIFT_MIN - 1))
		   << FSP_FLAGS_POS_PAGE_SSIZE)
		: FSP_FLAGS_PAGE_SSIZE();
	ut_ad(page_size_t(flags).equals_to(info.page_size));

	if (fil_space_create(dest_space_name, info.space_id, flags,
			      FIL_TYPE_TABLESPACE, 0)) {
		*success = xb_space_create_file(real_name, info.space_id,
						flags, &file);
	} else {
		msg("Can't create tablespace %s\n", dest_space_name);
	}

	goto exit;
}

/************************************************************************
Applies a given .delta file to the corresponding data file.
@return TRUE on success */
static
ibool
xtrabackup_apply_delta(
	const char*	dirname,	/* in: dir name of incremental */
	const char*	dbname,		/* in: database name (ibdata: NULL) */
	const char*	filename,	/* in: file name (not a path),
					including the .delta extension */
	void*		/*data*/)
{
	pfs_os_file_t	src_file;
	pfs_os_file_t	dst_file;
	char	src_path[FN_REFLEN];
	char	dst_path[FN_REFLEN];
	char	meta_path[FN_REFLEN];
	char	space_name[FN_REFLEN];
	bool	success;

	ibool	last_buffer = FALSE;
	ulint	page_in_buffer;
	ulint	incremental_buffers = 0;

	xb_delta_info_t info(univ_page_size, SRV_TMP_SPACE_ID);
	ulint		page_size;
	ulint		page_size_shift;
	byte*		incremental_buffer_base = NULL;
	byte*		incremental_buffer;

	size_t		offset;

	ut_a(xtrabackup_incremental);

	if (dbname) {
		snprintf(src_path, sizeof(src_path), "%s/%s/%s",
			 dirname, dbname, filename);
		snprintf(dst_path, sizeof(dst_path), "%s/%s/%s",
			 xtrabackup_real_target_dir, dbname, filename);
	} else {
		snprintf(src_path, sizeof(src_path), "%s/%s",
			 dirname, filename);
		snprintf(dst_path, sizeof(dst_path), "%s/%s",
			 xtrabackup_real_target_dir, filename);
	}
	dst_path[strlen(dst_path) - 6] = '\0';

	strncpy(space_name, filename, FN_REFLEN - 1);
	space_name[FN_REFLEN - 1] = '\0';
	space_name[strlen(space_name) -  6] = 0;

	if (!get_meta_path(src_path, meta_path)) {
		goto error;
	}

	os_normalize_path(dst_path);
	os_normalize_path(src_path);
	os_normalize_path(meta_path);

	if (!xb_read_delta_metadata(meta_path, &info)) {
		goto error;
	}

	page_size = info.page_size.physical();
	page_size_shift = get_bit_shift(page_size);
	msg("page size for %s is %zu bytes",
	    src_path, page_size);
	if (page_size_shift < 10 ||
	    page_size_shift > UNIV_PAGE_SIZE_SHIFT_MAX) {
		msg("error: invalid value of page_size "
		    "(%zu bytes) read from %s", page_size, meta_path);
		goto error;
	}

	src_file = os_file_create_simple_no_error_handling(
		0, src_path,
		OS_FILE_OPEN, OS_FILE_READ_WRITE, false, &success);
	if (!success) {
		os_file_get_last_error(TRUE);
		msg("error: can't open %s", src_path);
		goto error;
	}

	posix_fadvise(src_file, 0, 0, POSIX_FADV_SEQUENTIAL);

	dst_file = xb_delta_open_matching_space(
			dbname, space_name, info,
			dst_path, sizeof(dst_path), &success);
	if (!success) {
		msg("error: can't open %s", dst_path);
		goto error;
	}

	posix_fadvise(dst_file, 0, 0, POSIX_FADV_DONTNEED);

	/* allocate buffer for incremental backup (4096 pages) */
	incremental_buffer_base = static_cast<byte *>
		(malloc((page_size / 4 + 1) * page_size));
	incremental_buffer = static_cast<byte *>
		(ut_align(incremental_buffer_base,
			  page_size));

	msg("Applying %s to %s...", src_path, dst_path);

	while (!last_buffer) {
		ulint cluster_header;

		/* read to buffer */
		/* first block of block cluster */
		offset = ((incremental_buffers * (page_size / 4))
			 << page_size_shift);
		success = os_file_read(IORequestRead, src_file,
				       incremental_buffer, offset, page_size);
		if (success != DB_SUCCESS) {
			goto error;
		}

		cluster_header = mach_read_from_4(incremental_buffer);
		switch(cluster_header) {
			case 0x78747261UL: /*"xtra"*/
				break;
			case 0x58545241UL: /*"XTRA"*/
				last_buffer = TRUE;
				break;
			default:
				msg("error: %s seems not "
				    ".delta file.", src_path);
				goto error;
		}

		/* FIXME: If the .delta modifies FSP_SIZE on page 0,
		extend the file to that size. */

		for (page_in_buffer = 1; page_in_buffer < page_size / 4;
		     page_in_buffer++) {
			if (mach_read_from_4(incremental_buffer + page_in_buffer * 4)
			    == 0xFFFFFFFFUL)
				break;
		}

		ut_a(last_buffer || page_in_buffer == page_size / 4);

		/* read whole of the cluster */
		success = os_file_read(IORequestRead, src_file,
				       incremental_buffer,
				       offset, page_in_buffer * page_size);
		if (success != DB_SUCCESS) {
			goto error;
		}

		posix_fadvise(src_file, offset, page_in_buffer * page_size,
			      POSIX_FADV_DONTNEED);

		for (page_in_buffer = 1; page_in_buffer < page_size / 4;
		     page_in_buffer++) {
			ulint offset_on_page;

			offset_on_page = mach_read_from_4(incremental_buffer + page_in_buffer * 4);

			if (offset_on_page == 0xFFFFFFFFUL)
				break;

			uchar *buf = incremental_buffer + page_in_buffer * page_size;
			const os_offset_t off = os_offset_t(offset_on_page)*page_size;

			if (off == 0) {
				/* Read tablespace size from page 0,
				and extend the file to specified size.*/
				os_offset_t n_pages = mach_read_from_4(
					buf + FSP_HEADER_OFFSET + FSP_SIZE);
				if (mach_read_from_4(buf
						     + FIL_PAGE_SPACE_ID)) {
					if (!os_file_set_size(
						    dst_path, dst_file,
						    n_pages * page_size))
						goto error;
				} else if (fil_space_t* space
					   = fil_space_acquire(0)) {
					/* The system tablespace can
					consist of multiple files. The
					first one has full tablespace
					size in page 0, but only the last
					file should be extended. */
					fil_node_t* n = UT_LIST_GET_FIRST(
						space->chain);
					bool fail = !strcmp(n->name, dst_path)
						&& !fil_space_extend(
							space, (ulint)n_pages);
					fil_space_release(space);
					if (fail) goto error;
				}
			}

			success = os_file_write(IORequestWrite,
						dst_path, dst_file, buf, off, page_size);
			if (success != DB_SUCCESS) {
				goto error;
			}
		}

		/* Free file system buffer cache after the batch was written. */
#ifdef __linux__
		os_file_flush_func(dst_file);
#endif
		posix_fadvise(dst_file, 0, 0, POSIX_FADV_DONTNEED);


		incremental_buffers++;
	}

	free(incremental_buffer_base);
	if (src_file != OS_FILE_CLOSED) {
		os_file_close(src_file);
		os_file_delete(0,src_path);
	}
	if (dst_file != OS_FILE_CLOSED)
		os_file_close(dst_file);
	return TRUE;

error:
	free(incremental_buffer_base);
	if (src_file != OS_FILE_CLOSED)
		os_file_close(src_file);
	if (dst_file != OS_FILE_CLOSED)
		os_file_close(dst_file);
	msg("Error: xtrabackup_apply_delta(): "
	    "failed to apply %s to %s.\n", src_path, dst_path);
	return FALSE;
}


std::string change_extension(std::string filename, std::string new_ext) {
	DBUG_ASSERT(new_ext.size() == 3);
	std::string new_name(filename);
	new_name.resize(new_name.size() - new_ext.size());
	new_name.append(new_ext);
	return new_name;
}


static void rename_file(const char *from,const char *to) {
	msg("Renaming %s to %s\n", from, to);
	if (my_rename(from, to, MY_WME)) {
		die("Can't rename %s to %s errno %d", from, to, errno);
	}
}

static void rename_file(const std::string& from, const std::string &to) {
	rename_file(from.c_str(), to.c_str());
}
/************************************************************************
Callback to handle datadir entry. Function of this type will be called
for each entry which matches the mask by xb_process_datadir.
@return should return TRUE on success */
typedef ibool (*handle_datadir_entry_func_t)(
/*=========================================*/
	const char*	data_home_dir,		/*!<in: path to datadir */
	const char*	db_name,		/*!<in: database name */
	const char*	file_name,		/*!<in: file name with suffix */
	void*		arg);			/*!<in: caller-provided data */

/** Rename, and replace destination file, if exists */
static void rename_force(const char *from, const char *to) {
	if (access(to, R_OK) == 0) {
		msg("Removing %s", to);
		if (my_delete(to, MYF(MY_WME))) {
			msg("Can't remove %s, errno %d", to, errno);
			exit(EXIT_FAILURE);
		}
	}
	rename_file(from,to);
}


/** During prepare phase, rename ".new" files, that were created in
backup_fix_ddl() and backup_optimized_ddl_op(), to ".ibd". In the case of
incremental backup, i.e. of arg argument is set, move ".new" files to
destination directory and rename them to ".ibd", remove existing ".ibd.delta"
and ".idb.meta" files in incremental directory to avoid applying delta to
".ibd" file.

@param[in] data_home_dir	path to datadir
@param[in] db_name	database name
@param[in] file_name	file name with suffix
@param[in] arg	destination path, used in incremental backup to notify, that
*.new file must be moved to destibation directory

@return true */
static ibool prepare_handle_new_files(const char *data_home_dir,
                                      const char *db_name,
                                      const char *file_name, void *arg)
{
	const char *dest_dir = static_cast<const char *>(arg);
	std::string src_path = std::string(data_home_dir) + '/' + std::string(db_name) + '/';
	/* Copy "*.new" files from incremental to base dir for incremental backup */
	std::string dest_path=
		dest_dir ? std::string(dest_dir) + '/' + std::string(db_name) +
			'/' : src_path;

	/*
	  A CREATE DATABASE could have happened during the base mariabackup run.
	  In case if the current table file (e.g. `t1.new`) is from such
	  a new database, the database directory may not exist yet in
	  the base backup directory. Let's make sure to check if the directory
	  exists (and create if needed).
	*/
	if (!directory_exists(dest_path.c_str(), true/*create if not exists*/))
		return FALSE;
	src_path+= file_name;
	dest_path+= file_name;

	size_t index = dest_path.find(".new");
	DBUG_ASSERT(index != std::string::npos);
	dest_path.replace(index, strlen(".ibd"), ".ibd");
	rename_force(src_path.c_str(),dest_path.c_str());

	if (dest_dir) {
		/* remove delta and meta files to avoid delta applying for new file */
		index = src_path.find(".new");
		DBUG_ASSERT(index != std::string::npos);
		src_path.replace(index, std::string::npos, ".ibd.delta");
		if (access(src_path.c_str(), R_OK) == 0) {
			msg("Removing %s", src_path.c_str());
			if (my_delete(src_path.c_str(), MYF(MY_WME)))
				die("Can't remove %s, errno %d", src_path.c_str(), errno);
		}
		src_path.replace(index, std::string::npos, ".ibd.meta");
		if (access(src_path.c_str(), R_OK) == 0) {
			msg("Removing %s", src_path.c_str());
			if (my_delete(src_path.c_str(), MYF(MY_WME)))
				die("Can't remove %s, errno %d", src_path.c_str(), errno);
		}

		/* add table name to the container to avoid it's deletion at the end of
		 prepare */
		std::string table_name = std::string(db_name) + '/'
			+ std::string(file_name, file_name + strlen(file_name) - strlen(".new"));
		xb_filter_entry_t *table = static_cast<xb_filter_entry_t *>
			(malloc(sizeof(xb_filter_entry_t) + table_name.size() + 1));
		table->name = ((char*)table) + sizeof(xb_filter_entry_t);
		strcpy(table->name, table_name.c_str());
		HASH_INSERT(xb_filter_entry_t, name_hash, inc_dir_tables_hash,
				ut_fold_string(table->name), table);
	}

	return TRUE;
}

/************************************************************************
Callback to handle datadir entry. Deletes entry if it has no matching
fil_space in fil_system directory.
@return FALSE if delete attempt was unsuccessful */
static
ibool
rm_if_not_found(
	const char*	data_home_dir,		/*!<in: path to datadir */
	const char*	db_name,		/*!<in: database name */
	const char*	file_name,		/*!<in: file name with suffix */
	void*		arg __attribute__((unused)))
{
	char			name[FN_REFLEN];
	xb_filter_entry_t*	table;

	snprintf(name, FN_REFLEN, "%s/%s", db_name, file_name);
	/* Truncate ".ibd" */
	name[strlen(name) - 4] = '\0';

	HASH_SEARCH(name_hash, inc_dir_tables_hash, ut_fold_string(name),
		    xb_filter_entry_t*,
		    table, (void) 0,
		    !strcmp(table->name, name));

	if (!table) {
		snprintf(name, FN_REFLEN, "%s/%s/%s", data_home_dir,
						      db_name, file_name);
		return os_file_delete(0, name);
	}

	return(TRUE);
}

/** Function enumerates files in datadir (provided by path) which are matched
by provided suffix. For each entry callback is called.

@param[in] path	datadir path
@param[in] suffix	suffix to match against
@param[in] func	callback
@param[in] func_arg	arguments for the above callback

@return FALSE if callback for some entry returned FALSE */
static ibool xb_process_datadir(const char *path, const char *suffix,
                                handle_datadir_entry_func_t func,
                                void *func_arg = NULL)
{
	ulint		ret;
	char		dbpath[OS_FILE_MAX_PATH];
	os_file_dir_t	dir;
	os_file_dir_t	dbdir;
	os_file_stat_t	dbinfo;
	os_file_stat_t	fileinfo;
	ulint		suffix_len;
	dberr_t		err 		= DB_SUCCESS;
	static char	current_dir[2];

	current_dir[0] = FN_CURLIB;
	current_dir[1] = 0;
	srv_data_home = current_dir;

	suffix_len = strlen(suffix);

	/* datafile */
	dbdir = os_file_opendir(path, FALSE);

	if (dbdir != NULL) {
		ret = fil_file_readdir_next_file(&err, path, dbdir,
							&fileinfo);
		while (ret == 0) {
			if (fileinfo.type == OS_FILE_TYPE_DIR) {
				goto next_file_item_1;
			}

			if (strlen(fileinfo.name) > suffix_len
			    && 0 == strcmp(fileinfo.name + 
					strlen(fileinfo.name) - suffix_len,
					suffix)) {
				if (!func(
					    path, NULL,
					    fileinfo.name, func_arg))
				{
					os_file_closedir(dbdir);
					return(FALSE);
				}
			}
next_file_item_1:
			ret = fil_file_readdir_next_file(&err,
							path, dbdir,
							&fileinfo);
		}

		os_file_closedir(dbdir);
	} else {
		msg("Can't open dir %s", path);
	}

	/* single table tablespaces */
	dir = os_file_opendir(path, FALSE);

	if (dir == NULL) {
		msg("Can't open dir %s", path);
	}

		ret = fil_file_readdir_next_file(&err, path, dir,
								&dbinfo);
	while (ret == 0) {
		if (dbinfo.type == OS_FILE_TYPE_FILE
		    || dbinfo.type == OS_FILE_TYPE_UNKNOWN) {

		        goto next_datadir_item;
		}

		snprintf(dbpath, sizeof(dbpath), "%.*s/%.*s",
                         OS_FILE_MAX_PATH/2-1,
                         path,
                         OS_FILE_MAX_PATH/2-1,
                         dbinfo.name);

		os_normalize_path(dbpath);

		dbdir = os_file_opendir(dbpath, FALSE);

		if (dbdir != NULL) {

			ret = fil_file_readdir_next_file(&err, dbpath, dbdir,
								&fileinfo);
			while (ret == 0) {

			        if (fileinfo.type == OS_FILE_TYPE_DIR) {

				        goto next_file_item_2;
				}

				if (strlen(fileinfo.name) > suffix_len
				    && 0 == strcmp(fileinfo.name + 
						strlen(fileinfo.name) -
								suffix_len,
						suffix)) {
					/* The name ends in suffix; process
					the file */
					if (!func(
						    path,
						    dbinfo.name,
						    fileinfo.name, func_arg))
					{
						os_file_closedir(dbdir);
						os_file_closedir(dir);
						return(FALSE);
					}
				}
next_file_item_2:
				ret = fil_file_readdir_next_file(&err,
								dbpath, dbdir,
								&fileinfo);
			}

			os_file_closedir(dbdir);
		}
next_datadir_item:
		ret = fil_file_readdir_next_file(&err,
						path,
								dir, &dbinfo);
	}

	os_file_closedir(dir);

	return(TRUE);
}

/************************************************************************
Applies all .delta files from incremental_dir to the full backup.
@return TRUE on success. */
static
ibool
xtrabackup_apply_deltas()
{
	return xb_process_datadir(xtrabackup_incremental_dir, ".delta",
		xtrabackup_apply_delta);
}


static
void
innodb_free_param()
{
	srv_sys_space.shutdown();
	free_tmpdir(&mysql_tmpdir_list);
}


/** Check if file exists*/
static bool file_exists(std::string name)
{
	return access(name.c_str(), R_OK) == 0 ;
}

/** Read file content into STL string */
static std::string read_file_as_string(const std::string file) {
	char content[FN_REFLEN];
	FILE *f = fopen(file.c_str(), "r");
	if (!f) {
		msg("Can not open %s", file.c_str());
	}
	size_t len = fread(content, 1, FN_REFLEN, f);
	fclose(f);
	return std::string(content, len);
}

/** Delete file- Provide verbose diagnostics and exit, if operation fails. */
static void delete_file(const std::string& file, bool if_exists = false) {
	if (if_exists && !file_exists(file))
		return;
	if (my_delete(file.c_str(), MYF(MY_WME))) {
		die("Can't remove %s, errno %d", file.c_str(), errno);
	}
}

/**
Rename tablespace during prepare.
Backup in its end phase may generate some .ren files, recording
tablespaces that should be renamed in --prepare.
*/
static void rename_table_in_prepare(const std::string &datadir, const std::string& from , const std::string& to,
	const char *extension=0) {
	if (!extension) {
		static const char *extensions_nonincremental[] = { ".ibd", 0 };
		static const char *extensions_incremental[] = { ".ibd.delta", ".ibd.meta", 0 };
		const char **extensions = xtrabackup_incremental_dir ?
			extensions_incremental : extensions_nonincremental;
		for (size_t i = 0; extensions[i]; i++) {
			rename_table_in_prepare(datadir, from, to, extensions[i]);
		}
		return;
	}
	std::string src = std::string(datadir) + "/" + from + extension;
	std::string dest = std::string(datadir) + "/" + to + extension;
	std::string ren2, tmp;
	if (file_exists(dest)) {
		ren2= std::string(datadir) + "/" + to + ".ren";
		if (!file_exists(ren2)) {
			msg("ERROR : File %s was not found, but expected during rename processing\n", ren2.c_str());
			ut_a(0);
		}
		tmp = to + "#";
		rename_table_in_prepare(datadir, to, tmp);
	}
	rename_file(src, dest);
	if (ren2.size()) {
		// Make sure the temp. renamed file is processed.
		std::string to2 = read_file_as_string(ren2);
		rename_table_in_prepare(datadir, tmp, to2);
		delete_file(ren2);
	}
}

static ibool prepare_handle_ren_files(const char *datadir, const char *db, const char *filename, void *) {

	std::string ren_file = std::string(datadir) + "/" + db + "/" + filename;
	if (!file_exists(ren_file))
		return TRUE;

	std::string to = read_file_as_string(ren_file);
	std::string source_space_name = std::string(db) + "/" + filename;
	source_space_name.resize(source_space_name.size() - 4); // remove extension

	rename_table_in_prepare(datadir, source_space_name.c_str(), to.c_str());
	delete_file(ren_file);
	return TRUE;
}

/* Remove tablespaces during backup, based on */
static ibool prepare_handle_del_files(const char *datadir, const char *db, const char *filename, void *) {
	std::string del_file = std::string(datadir) + "/" + db + "/" + filename;
	std::string path(del_file);
	path.resize(path.size() - 4); // remove extension;
	if (xtrabackup_incremental) {
		delete_file(path + ".ibd.delta", true);
		delete_file(path + ".ibd.meta", true);
	}
	else {
		delete_file(path + ".ibd", true);
	}
	delete_file(del_file);
	return TRUE;
}

/** Implement --prepare
@return	whether the operation succeeded */
static bool xtrabackup_prepare_func(char** argv)
{
	CorruptedPages corrupted_pages;
	char			 metadata_path[FN_REFLEN];

	/* cd to target-dir */

	if (my_setwd(xtrabackup_real_target_dir,MYF(MY_WME)))
	{
		msg("can't my_setwd %s", xtrabackup_real_target_dir);
		return(false);
	}
	msg("cd to %s", xtrabackup_real_target_dir);

	fil_path_to_mysql_datadir = ".";

	ut_ad(xtrabackup_incremental == xtrabackup_incremental_dir);
	if (xtrabackup_incremental) {
		inc_dir_tables_hash = hash_create(1000);
		ut_ad(inc_dir_tables_hash);
	}

	msg("open files limit requested %u, set to %u",
	    (uint) xb_open_files_limit,
	    xb_set_max_open_files(xb_open_files_limit));

	/* Fix DDL for prepare. Process .del,.ren, and .new files.
	The order in which files are processed, is important
	(see MDEV-18185, MDEV-18201)
	*/
	xb_process_datadir(xtrabackup_incremental_dir ? xtrabackup_incremental_dir : ".",
		".del", prepare_handle_del_files);
	xb_process_datadir(xtrabackup_incremental_dir? xtrabackup_incremental_dir:".",
		".ren", prepare_handle_ren_files);
	if (xtrabackup_incremental_dir) {
		xb_process_datadir(xtrabackup_incremental_dir, ".new.meta", prepare_handle_new_files);
		xb_process_datadir(xtrabackup_incremental_dir, ".new.delta", prepare_handle_new_files);
		xb_process_datadir(xtrabackup_incremental_dir, ".new",
				prepare_handle_new_files, (void *)".");
	}
	else {
		xb_process_datadir(".", ".new", prepare_handle_new_files);
	}

	int argc; for (argc = 0; argv[argc]; argc++) {}
	encryption_plugin_prepare_init(argc, argv);

	xtrabackup_target_dir= mysql_data_home_buff;
	xtrabackup_target_dir[0]=FN_CURLIB;		// all paths are relative from here
	xtrabackup_target_dir[1]=0;
	const lsn_t target_lsn = xtrabackup_incremental
		? incremental_to_lsn : metadata_to_lsn;

	/*
	  read metadata of target
	*/
	sprintf(metadata_path, "%s/%s", xtrabackup_target_dir,
		XTRABACKUP_METADATA_FILENAME);

	if (!xtrabackup_read_metadata(metadata_path)) {
		msg("Error: failed to read metadata from '%s'\n",
		    metadata_path);
		return(false);
	}

	if (!strcmp(metadata_type, "full-backuped")) {
		if (xtrabackup_incremental) {
			msg("error: applying incremental backup "
			    "needs a prepared target.");
			return(false);
		}
		msg("This target seems to be not prepared yet.");
	} else if (!strcmp(metadata_type, "log-applied")) {
		msg("This target seems to be already prepared.");
	} else {
		msg("This target does not have correct metadata.");
		return(false);
	}

	bool ok = !xtrabackup_incremental
		|| metadata_to_lsn == incremental_lsn;
	if (!ok) {
		msg("error: This incremental backup seems "
		    "not to be proper for the target. Check 'to_lsn' of the target and "
		    "'from_lsn' of the incremental.");
		return(false);
	}

	srv_max_n_threads = 1000;
	srv_undo_logs = 1;
	srv_n_purge_threads = 1;

	xb_filters_init();

	srv_log_group_home_dir = NULL;
	srv_thread_concurrency = 1;

	if (xtrabackup_incremental) {
		srv_operation = SRV_OPERATION_RESTORE_DELTA;

		if (innodb_init_param()) {
			goto error_cleanup;
		}

		xb_normalize_init_values();
		sync_check_init();
		ut_d(sync_check_enable());
		ut_crc32_init();
		recv_sys_init();
		log_sys_init();
		recv_recovery_on = true;

#ifdef WITH_INNODB_DISALLOW_WRITES
		srv_allow_writes_event = os_event_create(0);
		os_event_set(srv_allow_writes_event);
#endif
		dberr_t err = xb_data_files_init();
		if (err != DB_SUCCESS) {
			msg("mariabackup: error: xb_data_files_init() failed "
			    "with error %s\n", ut_strerr(err));
			goto error_cleanup;
		}

		ok = xtrabackup_apply_deltas();

		xb_data_files_close();

		if (ok) {
			/* Cleanup datadir from tablespaces deleted
			between full and incremental backups */

			xb_process_datadir("./", ".ibd", rm_if_not_found);
		}

		xb_filter_hash_free(inc_dir_tables_hash);

		fil_close();
#ifdef WITH_INNODB_DISALLOW_WRITES
		os_event_destroy(srv_allow_writes_event);
#endif
		innodb_free_param();
		log_shutdown();
		sync_check_close();
		if (!ok) goto error_cleanup;
	}

        srv_operation=
            xtrabackup_export
                ? SRV_OPERATION_RESTORE_EXPORT
                : (xtrabackup_rollback_xa ? SRV_OPERATION_RESTORE_ROLLBACK_XA
                                          : SRV_OPERATION_RESTORE);

        if (innodb_init_param()) {
		goto error_cleanup;
	}

	/* increase IO threads */
	if (srv_n_file_io_threads < 10) {
		srv_n_read_io_threads = 4;
		srv_n_write_io_threads = 4;
	}

	msg("Starting InnoDB instance for recovery.");

	msg("mariabackup: Using %lld bytes for buffer pool "
	    "(set by --use-memory parameter)", xtrabackup_use_memory);

	srv_max_buf_pool_modified_pct = (double)max_buf_pool_modified_pct;

	if (srv_max_dirty_pages_pct_lwm > srv_max_buf_pool_modified_pct) {
		srv_max_dirty_pages_pct_lwm = srv_max_buf_pool_modified_pct;
	}

        if (xtrabackup_rollback_xa)
          srv_fast_shutdown= 0;

        if (innodb_init()) {
		goto error_cleanup;
	}

        corrupted_pages.read_from_file(MB_CORRUPTED_PAGES_FILE);
        if (xtrabackup_incremental)
        {
          char inc_filename[FN_REFLEN];
          sprintf(inc_filename, "%s/%s", xtrabackup_incremental_dir,
                  MB_CORRUPTED_PAGES_FILE);
          corrupted_pages.read_from_file(inc_filename);
        }
        if (!corrupted_pages.empty())
          corrupted_pages.zero_out_free_pages();
        if (corrupted_pages.empty())
        {
          if (!xtrabackup_incremental && unlink(MB_CORRUPTED_PAGES_FILE) &&
              errno != ENOENT)
          {
            char errbuf[MYSYS_STRERROR_SIZE];
            my_strerror(errbuf, sizeof(errbuf), errno);
            die("Error: unlink %s failed: %s", MB_CORRUPTED_PAGES_FILE,
                errbuf);
          }
        }
        else
          corrupted_pages.print_to_file(MB_CORRUPTED_PAGES_FILE);

        if (xtrabackup_rollback_xa)
        {
          /* Please do not merge MDEV-21168 fix in 10.5+ */
          compile_time_assert(MYSQL_VERSION_ID < 10 * 10000 + 5 * 100);
          XID *xid_list=
              (XID *) my_malloc(MAX_XID_LIST_SIZE * sizeof(XID), MYF(0));
          if (!xid_list)
          {
            msg("Can't allocate %i bytes for XID's list", MAX_XID_LIST_SIZE);
            ok= false;
            goto error_cleanup;
          }
          int got;
          ut_ad(recv_no_log_write);
          ut_d(recv_no_log_write= false);
          while ((got= trx_recover_for_mysql(xid_list, MAX_XID_LIST_SIZE)) > 0)
          {
            for (int i= 0; i < got; i++)
            {
#ifndef DBUG_OFF
              int rc=
#endif // !DBUG_OFF
                  innobase_rollback_by_xid(NULL, xid_list + i);
#ifndef DBUG_OFF
              if (rc == 0)
              {
                char buf[XIDDATASIZE * 4 + 6]; // see xid_to_str
                DBUG_PRINT("info",
                           ("rollback xid %s", xid_to_str(buf, xid_list[i])));
              }
#endif // !DBUG_OFF
            }
          }
          ut_d(recv_no_log_write= true);
          my_free(xid_list);
        }

        if (ok) {
		mtr_t			mtr;
		mtr.start();
		const trx_sysf_t*	sys_header = trx_sysf_get(&mtr);

		if (mach_read_from_4(TRX_SYS_MYSQL_LOG_INFO
				     + TRX_SYS_MYSQL_LOG_MAGIC_N_FLD
				     + sys_header)
		    == TRX_SYS_MYSQL_LOG_MAGIC_N) {
			ulonglong pos = mach_read_from_8(
				TRX_SYS_MYSQL_LOG_INFO
				+ TRX_SYS_MYSQL_LOG_OFFSET
				+ sys_header);
			const char* name = reinterpret_cast<const char*>(
				TRX_SYS_MYSQL_LOG_INFO + TRX_SYS_MYSQL_LOG_NAME
				+ sys_header);
			msg("Last binlog file %s, position %llu", name, pos);
		}

		mtr.commit();
	}

	/* Check whether the log is applied enough or not. */
	if ((srv_start_lsn || fil_space_get(SRV_LOG_SPACE_FIRST_ID))
	    && srv_start_lsn < target_lsn) {
		msg("mariabackup: error: "
		    "The log was only applied up to LSN " LSN_PF
		    ", instead of " LSN_PF,
		    srv_start_lsn, target_lsn);
		ok = false;
	}
#ifdef WITH_WSREP
	else if (ok) xb_write_galera_info(xtrabackup_incremental);
#endif

        if (xtrabackup_rollback_xa)
        {
          //	See	innobase_end() and thd_destructor_proxy()
          while (srv_fast_shutdown == 0 &&
                 (trx_sys_any_active_transactions() ||
                  (uint) thread_count > srv_n_purge_threads + 1))
          {
            os_thread_sleep(1000);
          }
          srv_shutdown_bg_undo_sources();
          srv_purge_shutdown();
          buf_flush_sync_all_buf_pools();
        }

        innodb_shutdown();
        innodb_free_param();

	/* output to metadata file */
	if (ok) {
		char	filename[FN_REFLEN];

		strcpy(metadata_type, "log-applied");

		if(xtrabackup_incremental
		   && metadata_to_lsn < incremental_to_lsn)
		{
			metadata_to_lsn = incremental_to_lsn;
			metadata_last_lsn = incremental_last_lsn;
		}

		sprintf(filename, "%s/%s", xtrabackup_target_dir, XTRABACKUP_METADATA_FILENAME);
		if (!xtrabackup_write_metadata(filename)) {

			msg("mariabackup: Error: failed to write metadata "
			    "to '%s'", filename);
			ok = false;
		} else if (xtrabackup_extra_lsndir) {
			sprintf(filename, "%s/%s", xtrabackup_extra_lsndir, XTRABACKUP_METADATA_FILENAME);
			if (!xtrabackup_write_metadata(filename)) {
				msg("mariabackup: Error: failed to write "
				    "metadata to '%s'", filename);
				ok = false;
			}
		}
	}

	if (ok) ok = apply_log_finish();

	if (ok && xtrabackup_export)
		ok= (prepare_export() == 0);

error_cleanup:
	xb_filters_free();
        return ok && !ib::error::was_logged() && corrupted_pages.empty();
}

/**************************************************************************
Append group name to xb_load_default_groups list. */
static
void
append_defaults_group(const char *group, const char *default_groups[],
		      size_t default_groups_size)
{
	uint i;
	bool appended = false;
	for (i = 0; i < default_groups_size - 1; i++) {
		if (default_groups[i] == NULL) {
			default_groups[i] = group;
			appended = true;
			break;
		}
	}
	ut_a(appended);
}

static const char*
normalize_privilege_target_name(const char* name)
{
	if (strcmp(name, "*") == 0) {
		return "\\*";
	}
	else {
		/* should have no regex special characters. */
		ut_ad(strpbrk(name, ".()[]*+?") == 0);
	}
	return name;
}

/******************************************************************//**
Check if specific privilege is granted.
Uses regexp magic to check if requested privilege is granted for given
database.table or database.* or *.*
or if user has 'ALL PRIVILEGES' granted.
@return true if requested privilege is granted, false otherwise. */
static bool
has_privilege(const std::list<std::string> &granted,
	const char* required,
	const char* db_name,
	const char* table_name)
{
	char buffer[1000];
	regex_t priv_re;
	regmatch_t tables_regmatch[1];
	bool result = false;

	db_name = normalize_privilege_target_name(db_name);
	table_name = normalize_privilege_target_name(table_name);

	int written = snprintf(buffer, sizeof(buffer),
		"GRANT .*(%s)|(ALL PRIVILEGES).* ON (\\*|`%s`)\\.(\\*|`%s`)",
		required, db_name, table_name);
	if (written < 0 || written == sizeof(buffer)
		|| regcomp(&priv_re, buffer, REG_EXTENDED)) {
		die("regcomp() failed for '%s'", buffer);
	}

	typedef std::list<std::string>::const_iterator string_iter;
	for (string_iter i = granted.begin(), e = granted.end(); i != e; ++i) {
		int res = regexec(&priv_re, i->c_str(),
			1, tables_regmatch, 0);

		if (res != REG_NOMATCH) {
			result = true;
			break;
		}
	}

	xb_regfree(&priv_re);
	return result;
}

enum {
	PRIVILEGE_OK = 0,
	PRIVILEGE_WARNING = 1,
	PRIVILEGE_ERROR = 2,
};

/******************************************************************//**
Check if specific privilege is granted.
Prints error message if required privilege is missing.
@return PRIVILEGE_OK if requested privilege is granted, error otherwise. */
static
int check_privilege(
	const std::list<std::string> &granted_priv, /* in: list of
							granted privileges*/
	const char* required,		/* in: required privilege name */
	const char* target_database,	/* in: required privilege target
						database name */
	const char* target_table,	/* in: required privilege target
						table name */
	int error = PRIVILEGE_ERROR)	/* in: return value if privilege
						is not granted */
{
	if (!has_privilege(granted_priv,
		required, target_database, target_table)) {
		msg("%s: missing required privilege %s on %s.%s",
			(error == PRIVILEGE_ERROR ? "Error" : "Warning"),
			required, target_database, target_table);
		return error;
	}
	return PRIVILEGE_OK;
}


/******************************************************************//**
Check DB user privileges according to the intended actions.

Fetches DB user privileges, determines intended actions based on
command-line arguments and prints missing privileges.
May terminate application with EXIT_FAILURE exit code.*/
static void
check_all_privileges()
{
	if (!mysql_connection) {
		/* Not connected, no queries is going to be executed. */
		return;
	}

	/* Fetch effective privileges. */
	std::list<std::string> granted_privileges;
	MYSQL_ROW row = 0;
	MYSQL_RES* result = xb_mysql_query(mysql_connection, "SHOW GRANTS",
		true);
	while ((row = mysql_fetch_row(result))) {
		granted_privileges.push_back(*row);
	}
	mysql_free_result(result);

	int check_result = PRIVILEGE_OK;

	/* FLUSH TABLES WITH READ LOCK */
	if (!opt_no_lock)
	{
		check_result |= check_privilege(
			granted_privileges,
			"RELOAD", "*", "*");
	}

	if (!opt_no_lock)
	{
		check_result |= check_privilege(
			granted_privileges,
		"PROCESS", "*", "*");
	}

	/* KILL ... */
	if ((!opt_no_lock && (opt_kill_long_queries_timeout || opt_lock_ddl_per_table))
		/* START SLAVE SQL_THREAD */
		/* STOP SLAVE SQL_THREAD */
		|| opt_safe_slave_backup) {
		check_result |= check_privilege(
			granted_privileges,
			"SUPER", "*", "*",
			PRIVILEGE_WARNING);
	}

	/* SHOW MASTER STATUS */
	/* SHOW SLAVE STATUS */
	if (opt_galera_info || opt_slave_info
		|| (opt_no_lock && opt_safe_slave_backup)) {
		check_result |= check_privilege(granted_privileges,
			"REPLICATION CLIENT", "*", "*",
			PRIVILEGE_WARNING);
	}

	if (check_result & PRIVILEGE_ERROR) {
		mysql_close(mysql_connection);
		msg("Current privileges, as reported by 'SHOW GRANTS': ");
		int n=1;
		for (std::list<std::string>::const_iterator it = granted_privileges.begin();
			it != granted_privileges.end();
			it++,n++) {
				msg("  %d.%s", n, it->c_str());
		}
		die("Insufficient privileges");
	}
}

bool
xb_init()
{
	const char *mixed_options[4] = {NULL, NULL, NULL, NULL};
	int n_mixed_options;

	/* sanity checks */

	if (opt_slave_info
		&& opt_no_lock
		&& !opt_safe_slave_backup) {
		msg("Error: --slave-info is used with --no-lock but "
			"without --safe-slave-backup. The binlog position "
			"cannot be consistent with the backup data.");
		return(false);
	}

	if (xtrabackup_backup && opt_rsync)
	{
		if (xtrabackup_stream_fmt)
		{
			msg("Error: --rsync doesn't work with --stream\n");
			return(false);
		}
		bool have_rsync = IF_WIN(false, (system("rsync --version > /dev/null 2>&1") == 0));
		if (!have_rsync)
		{
			msg("Error: rsync executable not found, cannot run backup with --rsync\n");
			return false;
		}
	}

	n_mixed_options = 0;

	if (opt_decompress) {
		mixed_options[n_mixed_options++] = "--decompress";
	}

	if (xtrabackup_copy_back) {
		mixed_options[n_mixed_options++] = "--copy-back";
	}

	if (xtrabackup_move_back) {
		mixed_options[n_mixed_options++] = "--move-back";
	}

	if (xtrabackup_prepare) {
		mixed_options[n_mixed_options++] = "--apply-log";
	}

	if (n_mixed_options > 1) {
		msg("Error: %s and %s are mutually exclusive\n",
			mixed_options[0], mixed_options[1]);
		return(false);
	}

	if (xtrabackup_backup) {
		if ((mysql_connection = xb_mysql_connect()) == NULL) {
			return(false);
		}

		if (!get_mysql_vars(mysql_connection)) {
			return(false);
		}
		if (opt_check_privileges) {
			check_all_privileges();
		}
		history_start_time = time(NULL);

	}

	return(true);
}


extern void init_signals(void);

#include <sql_locale.h>


void setup_error_messages()
{
  my_default_lc_messages = &my_locale_en_US;
	if (init_errmessage())
	  die("could not initialize error messages");
}

void
handle_options(int argc, char **argv, char ***argv_client, char ***argv_server)
{
	/* Setup some variables for Innodb.*/

	srv_operation = SRV_OPERATION_RESTORE;

	files_charset_info = &my_charset_utf8_general_ci;


	setup_error_messages();
	sys_var_init();
	plugin_mutex_init();
	mysql_prlock_init(key_rwlock_LOCK_system_variables_hash, &LOCK_system_variables_hash);
	opt_stack_trace = 1;
	test_flags |=  TEST_SIGINT;
	init_signals();
#ifndef _WIN32
	/* Exit process on SIGINT. */
	my_sigset(SIGINT, SIG_DFL);
#endif

	sf_leaking_memory = 1; /* don't report memory leaks on early exist */

	int i;
	int ho_error;

	char*	target_dir = NULL;
	bool	prepare = false;

	char	conf_file[FN_REFLEN];
	int	argc_client = argc;
	int	argc_server = argc;

	/* scan options for group and config file to load defaults from */
	for (i = 1; i < argc; i++) {

		char *optend = strcend(argv[i], '=');

		if (strncmp(argv[i], "--defaults-group",
			    optend - argv[i]) == 0) {
			defaults_group = optend + 1;
			append_defaults_group(defaults_group,
				xb_server_default_groups,
				array_elements(xb_server_default_groups));
		}

		if (strncmp(argv[i], "--login-path",
			    optend - argv[i]) == 0) {
			append_defaults_group(optend + 1,
				xb_client_default_groups,
				array_elements(xb_client_default_groups));
		}

		if (!strncmp(argv[i], "--prepare",
			     optend - argv[i])) {
			prepare = true;
		}

		if (!strncmp(argv[i], "--apply-log",
			     optend - argv[i])) {
			prepare = true;
		}

		if (!strncmp(argv[i], "--target-dir",
			     optend - argv[i]) && *optend) {
			target_dir = optend + 1;
		}

		if (!*optend && argv[i][0] != '-') {
			target_dir = argv[i];
		}
	}

	snprintf(conf_file, sizeof(conf_file), "my");

	if (prepare && target_dir) {
		snprintf(conf_file, sizeof(conf_file),
			 "%s/backup-my.cnf", target_dir);
			if (!strncmp(argv[1], "--defaults-file=", 16)) {
				/* Remove defaults-file*/
				for (int i = 2; ; i++) {
					if ((argv[i-1]= argv[i]) == 0)
						break;
				}
				argc--;
			}
	}

	*argv_client = argv;
	*argv_server = argv;
	load_defaults_or_exit(conf_file, xb_server_default_groups,
			      &argc_server, argv_server);

	int n;
	for (n = 0; (*argv_server)[n]; n++) {};
	argc_server = n;

	print_param_str <<
		"# This MySQL options file was generated by XtraBackup.\n"
		"[" << defaults_group << "]\n";

	/* We want xtrabackup to ignore unknown options, because it only
	recognizes a small subset of server variables */
	my_getopt_skip_unknown = TRUE;

	/* Reset u_max_value for all options, as we don't want the
	--maximum-... modifier to set the actual option values */
	for (my_option *optp= xb_server_options; optp->name; optp++) {
		optp->u_max_value = (G_PTR *) &global_max_value;
	}


	/* Throw a descriptive error if --defaults-file or --defaults-extra-file
	is not the first command line argument */
	for (int i = 2 ; i < argc ; i++) {
		char *optend = strcend((argv)[i], '=');

		if (optend - argv[i] == 15 &&
			!strncmp(argv[i], "--defaults-file", optend - argv[i])) {
			die("--defaults-file must be specified first on the command line");
		}
		if (optend - argv[i] == 21 &&
			!strncmp(argv[i], "--defaults-extra-file",
				optend - argv[i])) {
			die("--defaults-extra-file must be specified first on the command line");
		}
	}

	if (argc_server > 0
	    && (ho_error=handle_options(&argc_server, argv_server,
					xb_server_options, xb_get_one_option)))
		exit(ho_error);

	load_defaults_or_exit(conf_file, xb_client_default_groups,
			      &argc_client, argv_client);

	for (n = 0; (*argv_client)[n]; n++) {};
 	argc_client = n;

	if (innobackupex_mode && argc_client > 0) {
		/* emulate innobackupex script */
		innobackupex_mode = true;
		if (!ibx_handle_options(&argc_client, argv_client)) {
			exit(EXIT_FAILURE);
		}
	}

	if (argc_client > 0
	    && (ho_error=handle_options(&argc_client, argv_client,
					xb_client_options, xb_get_one_option)))
		exit(ho_error);

	/* Reject command line arguments that don't look like options, i.e. are
	not of the form '-X' (single-character options) or '--option' (long
	options) */
	for (int i = 0 ; i < argc_client ; i++) {
		const char * const opt = (*argv_client)[i];

		if (strncmp(opt, "--", 2) &&
		    !(strlen(opt) == 2 && opt[0] == '-')) {
			bool server_option = true;

			for (int j = 0; j < argc_server; j++) {
				if (opt == (*argv_server)[j]) {
					server_option = false;
					break;
				}
			}

			if (!server_option) {
				msg("mariabackup: Error:"
				    " unknown argument: '%s'", opt);
				exit(EXIT_FAILURE);
			}
		}
	}
}

static int main_low(char** argv);
static int get_exepath(char *buf, size_t size, const char *argv0);

/* ================= main =================== */
int main(int argc, char **argv)
{
	char **client_defaults, **server_defaults;

	my_getopt_prefix_matching= 0;

	if (get_exepath(mariabackup_exe,FN_REFLEN, argv[0]))
    strncpy(mariabackup_exe,argv[0], FN_REFLEN-1);


	if (argc > 1 )
	{
		/* In "prepare export", we need  to start mysqld 
		Since it is not always be installed on the machine,
		we start "mariabackup --mysqld", which acts as mysqld
		*/
		if (strcmp(argv[1], "--mysqld") == 0)
		{
			extern int mysqld_main(int argc, char **argv);
			argc--;
			argv++;
			argv[0]+=2;
			return mysqld_main(argc, argv);
		}
		if(strcmp(argv[1], "--innobackupex") == 0)
		{
			argv++;
			argc--;
			innobackupex_mode = true;
		}
	}
  
	if (argc > 1)
		strncpy(orig_argv1,argv[1],sizeof(orig_argv1) -1);

	init_signals();
	MY_INIT(argv[0]);

	pthread_key_create(&THR_THD, NULL);
	my_pthread_setspecific_ptr(THR_THD, NULL);

	xb_regex_init();

	capture_tool_command(argc, argv);

	if (mysql_server_init(-1, NULL, NULL))
	{
		die("mysql_server_init() failed");
	}

	system_charset_info = &my_charset_utf8_general_ci;
	key_map_full.set_all();

	logger.init_base();
	logger.set_handlers(LOG_FILE, LOG_NONE, LOG_NONE);
	mysql_mutex_init(key_LOCK_error_log, &LOCK_error_log,
			 MY_MUTEX_INIT_FAST);

	handle_options(argc, argv, &client_defaults, &server_defaults);

#ifndef DBUG_OFF
	if (dbug_option) {
		DBUG_SET_INITIAL(dbug_option);
		DBUG_SET(dbug_option);
	}
#endif

	int status = main_low(server_defaults);

	backup_cleanup();

	if (innobackupex_mode) {
		ibx_cleanup();
	}

	free_defaults(client_defaults);
	free_defaults(server_defaults);

#ifndef DBUG_OFF
	if (dbug_option) {
		DBUG_END();
	}
#endif

	if (THR_THD)
		(void) pthread_key_delete(THR_THD);

	logger.cleanup_base();
	cleanup_errmsgs();
	free_error_messages();
	mysql_mutex_destroy(&LOCK_error_log);

	if (status == EXIT_SUCCESS) {
		msg("completed OK!");
	}

	return status;
}

static int main_low(char** argv)
{
	if (innobackupex_mode) {
		if (!ibx_init()) {
			return(EXIT_FAILURE);
		}
	}

	if (!xtrabackup_print_param && !xtrabackup_prepare
	    && !strcmp(mysql_data_home, "./")) {
		if (!xtrabackup_print_param)
			usage();
		msg("mariabackup: Error: Please set parameter 'datadir'");
		return(EXIT_FAILURE);
	}

	/* Expand target-dir, incremental-basedir, etc. */

	char cwd[FN_REFLEN];
	my_getwd(cwd, sizeof(cwd), MYF(0));

	my_load_path(xtrabackup_real_target_dir,
		     xtrabackup_target_dir, cwd);
	unpack_dirname(xtrabackup_real_target_dir,
		       xtrabackup_real_target_dir);
	xtrabackup_target_dir= xtrabackup_real_target_dir;

	if (xtrabackup_incremental_basedir) {
		my_load_path(xtrabackup_real_incremental_basedir,
			     xtrabackup_incremental_basedir, cwd);
		unpack_dirname(xtrabackup_real_incremental_basedir,
			       xtrabackup_real_incremental_basedir);
		xtrabackup_incremental_basedir =
			xtrabackup_real_incremental_basedir;
	}

	if (xtrabackup_incremental_dir) {
		my_load_path(xtrabackup_real_incremental_dir,
			     xtrabackup_incremental_dir, cwd);
		unpack_dirname(xtrabackup_real_incremental_dir,
			       xtrabackup_real_incremental_dir);
		xtrabackup_incremental_dir = xtrabackup_real_incremental_dir;
	}

	if (xtrabackup_extra_lsndir) {
		my_load_path(xtrabackup_real_extra_lsndir,
			     xtrabackup_extra_lsndir, cwd);
		unpack_dirname(xtrabackup_real_extra_lsndir,
			       xtrabackup_real_extra_lsndir);
		xtrabackup_extra_lsndir = xtrabackup_real_extra_lsndir;
	}

	/* get default temporary directory */
	if (!opt_mysql_tmpdir || !opt_mysql_tmpdir[0]) {
		opt_mysql_tmpdir = getenv("TMPDIR");
#if defined(__WIN__)
		if (!opt_mysql_tmpdir) {
			opt_mysql_tmpdir = getenv("TEMP");
		}
		if (!opt_mysql_tmpdir) {
			opt_mysql_tmpdir = getenv("TMP");
		}
#endif
		if (!opt_mysql_tmpdir || !opt_mysql_tmpdir[0]) {
			opt_mysql_tmpdir = const_cast<char*>(DEFAULT_TMPDIR);
		}
	}

	/* temporary setting of enough size */
	srv_page_size_shift = UNIV_PAGE_SIZE_SHIFT_MAX;
	srv_page_size = UNIV_PAGE_SIZE_MAX;
	if (xtrabackup_backup && xtrabackup_incremental) {
		/* direct specification is only for --backup */
		/* and the lsn is prior to the other option */

		char* endchar;
		int error = 0;
		incremental_lsn = strtoll(xtrabackup_incremental, &endchar, 10);
		if (*endchar != '\0')
			error = 1;

		if (error) {
			msg("mariabackup: value '%s' may be wrong format for "
			    "incremental option.", xtrabackup_incremental);
			return(EXIT_FAILURE);
		}
	} else if (xtrabackup_backup && xtrabackup_incremental_basedir) {
		char	filename[FN_REFLEN];

		sprintf(filename, "%s/%s", xtrabackup_incremental_basedir, XTRABACKUP_METADATA_FILENAME);

		if (!xtrabackup_read_metadata(filename)) {
			msg("mariabackup: error: failed to read metadata from "
			    "%s", filename);
			return(EXIT_FAILURE);
		}

		incremental_lsn = metadata_to_lsn;
		xtrabackup_incremental = xtrabackup_incremental_basedir; //dummy
	} else if (xtrabackup_prepare && xtrabackup_incremental_dir) {
		char	filename[FN_REFLEN];

		sprintf(filename, "%s/%s", xtrabackup_incremental_dir, XTRABACKUP_METADATA_FILENAME);

		if (!xtrabackup_read_metadata(filename)) {
			msg("mariabackup: error: failed to read metadata from "
			    "%s", filename);
			return(EXIT_FAILURE);
		}

		incremental_lsn = metadata_from_lsn;
		incremental_to_lsn = metadata_to_lsn;
		incremental_last_lsn = metadata_last_lsn;
		xtrabackup_incremental = xtrabackup_incremental_dir; //dummy

	} else if (opt_incremental_history_name) {
		xtrabackup_incremental = opt_incremental_history_name;
	} else if (opt_incremental_history_uuid) {
		xtrabackup_incremental = opt_incremental_history_uuid;
	} else {
		xtrabackup_incremental = NULL;
	}

	if (xtrabackup_stream && !xtrabackup_backup) {
		msg("Warning: --stream parameter is ignored, it only works together with --backup.");
	}

	if (!xb_init()) {
		return(EXIT_FAILURE);
	}

	/* --print-param */
	if (xtrabackup_print_param) {
		printf("%s", print_param_str.str().c_str());
		return(EXIT_SUCCESS);
	}

	print_version();
	if (xtrabackup_incremental) {
		msg("incremental backup from " LSN_PF " is enabled.",
		    incremental_lsn);
	}

	if (xtrabackup_export && innobase_file_per_table == FALSE) {
		msg("mariabackup: auto-enabling --innodb-file-per-table due to "
		    "the --export option");
		innobase_file_per_table = TRUE;
	}

	/* cannot execute both for now */
	{
		int num = 0;

		if (xtrabackup_backup) num++;
		if (xtrabackup_prepare) num++;
		if (xtrabackup_copy_back) num++;
		if (xtrabackup_move_back) num++;
		if (xtrabackup_decrypt_decompress) num++;
		if (num != 1) { /* !XOR (for now) */
			usage();
			return(EXIT_FAILURE);
		}
	}

	/* --backup */
	if (xtrabackup_backup && !xtrabackup_backup_func()) {
		return(EXIT_FAILURE);
	}

	/* --prepare */
	if (xtrabackup_prepare
	    && !xtrabackup_prepare_func(argv)) {
		return(EXIT_FAILURE);
	}

	if (xtrabackup_copy_back || xtrabackup_move_back) {
		if (!check_if_param_set("datadir")) {
			mysql_data_home = get_default_datadir();
		}
		if (!copy_back())
			return(EXIT_FAILURE);
	}

	if (xtrabackup_decrypt_decompress && !decrypt_decompress()) {
		return(EXIT_FAILURE);
	}

	return(EXIT_SUCCESS);
}


static int get_exepath(char *buf, size_t size, const char *argv0)
{
#ifdef _WIN32
  DWORD ret = GetModuleFileNameA(NULL, buf, size);
  if (ret > 0)
    return 0;
#elif defined(__linux__)
  ssize_t ret = readlink("/proc/self/exe", buf, size-1);
  if(ret > 0)
    return 0;
#endif

  return my_realpath(buf, argv0, 0);
}


#if defined (__SANITIZE_ADDRESS__) && defined (__linux__)
/* Avoid LeakSanitizer's false positives. */
const char* __asan_default_options()
{
  return "detect_leaks=0";
}
#endif
