/******************************************************
MariaBackup: hot backup tool for InnoDB
(c) 2009-2017 Percona LLC and/or its affiliates
Originally Created 3/3/2009 Yasufumi Kinoshita
Written by Alexey Kopytov, Aleksandr Kuzminsky, Stewart Smith, Vadim Tkachenko,
Yasufumi Kinoshita, Ignacio Nin and Baron Schwartz.
(c) 2017, 2022, MariaDB Corporation.
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

#include <my_global.h>
#include <my_config.h>
#include <unireg.h>
#include <mysql_version.h>
#include <my_base.h>
#include <my_getopt.h>
#include <mysql_com.h>
#include <my_default.h>
#include <scope.h>
#include <sql_class.h>

#include <fcntl.h>
#include <string.h>

#ifdef __linux__
# include <sys/prctl.h>
# include <sys/resource.h>
#endif

#ifdef __APPLE__
# include "libproc.h"
#endif

#ifdef __FreeBSD__
# include <sys/sysctl.h>
#endif


#include <btr0sea.h>
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
#include "xb_plugin.h"
#include <sql_plugin.h>
#include <srv0srv.h>
#include <log.h>
#include <derror.h>
#include <thr_timer.h>
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
my_bool xtrabackup_mysqld_args;
my_bool xtrabackup_help;

my_bool xtrabackup_export;

longlong xtrabackup_use_memory;

uint opt_protocol;
long xtrabackup_throttle; /* 0:unlimited */
static lint io_ticket;
static mysql_cond_t wait_throttle;
static mysql_cond_t log_copying_stop;

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

static hash_table_t tables_include_hash;
static hash_table_t tables_exclude_hash;

char *xtrabackup_databases = NULL;
char *xtrabackup_databases_file = NULL;
char *xtrabackup_databases_exclude = NULL;
static hash_table_t databases_include_hash;
static hash_table_t databases_exclude_hash;

static hash_table_t inc_dir_tables_hash;

struct xb_filter_entry_t{
	char*		name;
	ibool		has_tables;
	xb_filter_entry_t *name_hash;
};

static bool log_copying_running;

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
extern char *opt_tls_version;
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
ulong innobase_read_io_threads = 4;
ulong innobase_write_io_threads = 4;

longlong innobase_page_size = (1LL << 14); /* 16KB */
char *innobase_buffer_pool_filename = NULL;
char *buffer_pool_filename = NULL;

/* The default values for the following char* start-up parameters
are determined in innobase_init below: */

static char*	innobase_ignored_opt;
char*	innobase_data_home_dir;
char*	innobase_data_file_path;


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

extern const char *innodb_checksum_algorithm_names[];
extern TYPELIB innodb_checksum_algorithm_typelib;
extern const char *innodb_flush_method_names[];
extern TYPELIB innodb_flush_method_typelib;

static const char *binlog_info_values[] = {"off", "lockless", "on", "auto",
					   NullS};
static TYPELIB binlog_info_typelib = {array_elements(binlog_info_values)-1, "",
				      binlog_info_values, NULL};
ulong opt_binlog_info;

char *opt_incremental_history_name;
char *opt_incremental_history_uuid;

char *opt_user;
const char *opt_password;
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
uint opt_max_binlogs = UINT_MAX;

const char *opt_history = NULL;


char mariabackup_exe[FN_REFLEN];
char orig_argv1[FN_REFLEN];

pthread_cond_t  scanned_lsn_cond;

/** Store the deferred tablespace name during --backup */
static std::set<std::string> defer_space_names;

typedef std::map<space_id_t,std::string> space_id_to_name_t;

struct ddl_tracker_t {
	/** Tablspaces with their ID and name, as they were copied to backup.*/
	space_id_to_name_t tables_in_backup;
	/** Drop operations found in redo log. */
	std::set<space_id_t> drops;
	/* For DDL operation found in redo log,  */
	space_id_to_name_t id_to_name;
	/** Deferred tablespaces with their ID and name which was
	found in redo log of DDL operations */
	space_id_to_name_t deferred_tables;

  /** Insert the deferred tablespace id with the name */
  void insert_defer_id(space_id_t space_id, std::string name)
  {
    auto it= defer_space_names.find(name);
    if (it != defer_space_names.end())
    {
      deferred_tables[space_id]= name;
      defer_space_names.erase(it);
    }
  }

  /** Rename the deferred tablespace with new name */
  void rename_defer(space_id_t space_id, std::string old_name,
                    std::string new_name)
  {
    if (deferred_tables.find(space_id) != deferred_tables.end())
      deferred_tables[space_id] = new_name;
    auto defer_end= defer_space_names.end();
    auto defer= defer_space_names.find(old_name);
    if (defer == defer_end)
      defer= defer_space_names.find(new_name);

    if (defer != defer_end)
    {
      deferred_tables[space_id]= new_name;
      defer_space_names.erase(defer);
    }
  }

  /** Delete the deferred tablespace */
  void delete_defer(space_id_t space_id, std::string name)
  {
    deferred_tables.erase(space_id);
    defer_space_names.erase(name);
  }
};

static ddl_tracker_t ddl_tracker;

/** Stores the space ids of page0 INIT_PAGE redo records. It is
used to indicate whether the given deferred tablespace can
be reconstructed. */
static std::set<space_id_t> first_page_init_ids;

// Convert non-null terminated filename to space name
static std::string filename_to_spacename(const void *filename, size_t len);

CorruptedPages::CorruptedPages() { ut_a(!pthread_mutex_init(&m_mutex, NULL)); }

CorruptedPages::~CorruptedPages() { ut_a(!pthread_mutex_destroy(&m_mutex)); }

void CorruptedPages::add_page_no_lock(const char *space_name,
                                      page_id_t page_id,
                                      bool convert_space_name)
{
  space_info_t  &space_info = m_spaces[page_id.space()];
  if (space_info.space_name.empty())
    space_info.space_name= convert_space_name
      ? filename_to_spacename(space_name, strlen(space_name))
      : space_name;
  (void)space_info.pages.insert(page_id.page_no());
}

void CorruptedPages::add_page(const char *file_name, page_id_t page_id)
{
  pthread_mutex_lock(&m_mutex);
  add_page_no_lock(file_name, page_id, true);
  pthread_mutex_unlock(&m_mutex);
}

bool CorruptedPages::contains(page_id_t page_id) const
{
  bool result = false;
  ut_a(!pthread_mutex_lock(&m_mutex));
  container_t::const_iterator space_it= m_spaces.find(page_id.space());
  if (space_it != m_spaces.end())
    result = space_it->second.pages.count(page_id.page_no());
  ut_a(!pthread_mutex_unlock(&m_mutex));
  return result;
}

void CorruptedPages::drop_space(uint32_t space_id)
{
  ut_a(!pthread_mutex_lock(&m_mutex));
  m_spaces.erase(space_id);
  ut_a(!pthread_mutex_unlock(&m_mutex));
}

void CorruptedPages::rename_space(uint32_t space_id,
                                  const std::string &new_name)
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
    for (std::set<unsigned>::const_iterator page_it=
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
                                 static_cast<int>(out.str().size()));
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
  uint32_t space_id;
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
      std::istringstream iss(line);
      unsigned page_no;
      while ((iss >> page_no))
        add_page_no_lock(space_name.c_str(), {space_id, page_no}, false);
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
                                            bool set_size,
                                            uint32_t defer_space_id=0);
static void xb_data_files_close();
static fil_space_t* fil_space_get_by_name(const char* name);

void CorruptedPages::zero_out_free_pages()
{
  container_t non_free_pages;
  byte *zero_page=
      static_cast<byte *>(aligned_malloc(srv_page_size, srv_page_size));
  memset(zero_page, 0, srv_page_size);

  ut_a(!pthread_mutex_lock(&m_mutex));
  for (container_t::const_iterator space_it= m_spaces.begin();
       space_it != m_spaces.end(); ++space_it)
  {
    uint32_t space_id = space_it->first;
    const std::string &space_name = space_it->second.space_name;
    // There is no need to close tablespaces explixitly as they will be closed
    // in innodb_shutdown().
    xb_load_single_table_tablespace(space_name, false);
    fil_space_t *space = fil_space_t::get(space_id);
    if (!space)
      die("Can't find space object for space name %s to check corrupted page",
          space_name.c_str());
    for (std::set<unsigned>::const_iterator page_it=
             space_it->second.pages.begin();
         page_it != space_it->second.pages.end(); ++page_it)
    {
      bool is_free= fseg_page_is_free(space, *page_it);
      if (!is_free) {
        space_info_t &space_info = non_free_pages[space_id];
        space_info.pages.insert(*page_it);
        if (space_info.space_name.empty())
          space_info.space_name = space_name;
        msg("Error: corrupted page " UINT32PF
            " of tablespace %s can not be fixed",
            *page_it, space_name.c_str());
      }
      else
      {
        space->reacquire();
        auto err= space
                      ->io(IORequest(IORequest::PUNCH_RANGE),
                           *page_it * srv_page_size, srv_page_size, zero_page)
                      .err;
        if (err != DB_SUCCESS)
          die("Can't zero out corrupted page " UINT32PF " of tablespace %s",
              *page_it, space_name.c_str());
        msg("Corrupted page " UINT32PF
            " of tablespace %s was successfully fixed.",
            *page_it, space_name.c_str());
      }
    }
    space->flush<true>();
    space->release();
  }
  m_spaces.swap(non_free_pages);
  ut_a(!pthread_mutex_unlock(&m_mutex));
  aligned_free(zero_page);
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
                                                 bool skip_node_page0,
                                                 uint32_t defer_space_id);
static dberr_t enumerate_ibd_files(process_single_tablespace_func_t callback);

/* ======== Datafiles iterator ======== */
struct datafiles_iter_t {
	space_list_t::iterator space = fil_system.space_list.end();
	fil_node_t	*node = nullptr;
	bool		started = false;
	std::mutex	mutex;
};

/* ======== Datafiles iterator ======== */
static
fil_node_t *
datafiles_iter_next(datafiles_iter_t *it)
{
	fil_node_t *new_node;

	std::lock_guard<std::mutex> _(it->mutex);

	if (it->node == NULL) {
		if (it->started)
			goto end;
		it->started = TRUE;
	} else {
		it->node = UT_LIST_GET_NEXT(chain, it->node);
		if (it->node != NULL)
			goto end;
	}

	it->space = (it->space == fil_system.space_list.end()) ?
		fil_system.space_list.begin() :
		std::next(it->space);

	while (it->space != fil_system.space_list.end() &&
	       (it->space->purpose != FIL_TYPE_TABLESPACE ||
		UT_LIST_GET_LEN(it->space->chain) == 0))
		++it->space;
	if (it->space == fil_system.space_list.end())
		goto end;

	it->node = UT_LIST_GET_FIRST(it->space->chain);

end:
	new_node = it->node;

	return new_node;
}

#ifndef DBUG_OFF
struct dbug_thread_param_t
{
	MYSQL *con;
	const char *query;
	int expect_err;
	int expect_errno;
};


/* Thread procedure used in dbug_start_query_thread. */
static void *dbug_execute_in_new_connection(void *arg)
{
	mysql_thread_init();
	dbug_thread_param_t *par= static_cast<dbug_thread_param_t*>(arg);
	int err = mysql_query(par->con, par->query);
	int err_no = mysql_errno(par->con);
	if(par->expect_err != err)
	{
		msg("FATAL: dbug_execute_in_new_connection : mysql_query '%s' returns %d, instead of expected %d",
			par->query, err, par->expect_err);
		_exit(1);
	}
	if (err && par->expect_errno && par->expect_errno != err_no)
	{
		msg("FATAL: dbug_execute_in_new_connection: mysql_query '%s' returns mysql_errno %d, instead of expected %d",
			par->query, err_no, par->expect_errno);
		_exit(1);
	}
	mysql_close(par->con);
	mysql_thread_end();
	delete par;
	return nullptr;
}

static pthread_t dbug_alter_thread;

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
static void dbug_start_query_thread(
	const char *query,
	const char *wait_state,
	int expected_err,
	int expected_errno)

{
	dbug_thread_param_t *par = new dbug_thread_param_t;
	par->query = query;
	par->expect_err = expected_err;
	par->expect_errno = expected_errno;
	par->con =  xb_mysql_connect();

	mysql_thread_create(0, &dbug_alter_thread, nullptr,
			    dbug_execute_in_new_connection, par);

	if (!wait_state)
		return;

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
}
#endif

void mdl_lock_all()
{
  mdl_lock_init();
  datafiles_iter_t it;

  while (fil_node_t *node= datafiles_iter_next(&it))
  {
    const auto id= node->space->id;
    if (const char *name= (fil_is_user_tablespace_id(id) &&
                           node->space->chain.start)
        ? node->space->chain.start->name : nullptr)
      if (check_if_skip_table(filename_to_spacename(name,
                                                    strlen(name)).c_str()))
        continue;
    mdl_lock_table(id);
  }
}


// Convert non-null terminated filename to space name
static std::string filename_to_spacename(const void *filename, size_t len)
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
	*table = '/';
	std::string s(db ? db+1 : f);
	free(f);
	return s;
}

/** Report an operation to create, delete, or rename a file during backup.
@param[in]	space_id	tablespace identifier
@param[in]	type		redo log file operation type
@param[in]	name		file name (not NUL-terminated)
@param[in]	len		length of name, in bytes
@param[in]	new_name	new file name (NULL if not rename)
@param[in]	new_len		length of new_name, in bytes (0 if NULL) */
static void backup_file_op(uint32_t space_id, int type,
	const byte* name, ulint len,
	const byte* new_name, ulint new_len)
{

	ut_ad(name);
	ut_ad(len);
	ut_ad(!new_name == !new_len);
	mysql_mutex_assert_owner(&recv_sys.mutex);

	switch(type) {
	case FILE_CREATE:
	{
		std::string space_name = filename_to_spacename(name, len);
		ddl_tracker.id_to_name[space_id] = space_name;
		ddl_tracker.delete_defer(space_id, space_name);
		msg("DDL tracking : create %u \"%.*s\"", space_id, int(len), name);
	}
	break;
	case FILE_MODIFY:
		ddl_tracker.insert_defer_id(
			space_id, filename_to_spacename(name, len));
		msg("DDL tracking : modify %u \"%.*s\"", space_id, int(len), name);
		break;
	case FILE_RENAME:
	{
		std::string new_space_name = filename_to_spacename(
						new_name, new_len);
		std::string old_space_name = filename_to_spacename(
						name, len);
		ddl_tracker.id_to_name[space_id] = new_space_name;
		ddl_tracker.rename_defer(space_id, old_space_name,
					 new_space_name);
		msg("DDL tracking : rename %u \"%.*s\",\"%.*s\"",
			space_id, int(len), name, int(new_len), new_name);
	}
	break;
	case FILE_DELETE:
		ddl_tracker.drops.insert(space_id);
		ddl_tracker.delete_defer(
			space_id, filename_to_spacename(name, len));
		msg("DDL tracking : delete %u \"%.*s\"", space_id, int(len), name);
		break;
	default:
		ut_ad(0);
		break;
	}
}


/*
 This callback is called if DDL operation is detected,
 at the end of backup

 Normally, DDL operations are blocked due to FTWRL,
 but in rare cases of --no-lock, they are not.

 We will abort backup in this case.
*/
static void backup_file_op_fail(uint32_t space_id, int type,
	const byte* name, ulint len,
	const byte* new_name, ulint new_len)
{
	bool fail = false;
	switch(type) {
	case FILE_CREATE:
		msg("DDL tracking : create %u \"%.*s\"", space_id, int(len), name);
		fail = !check_if_skip_table(
				filename_to_spacename(name, len).c_str());
		break;
	case FILE_MODIFY:
		msg("DDL tracking : modify %u \"%.*s\"", space_id, int(len), name);
		break;
	case FILE_RENAME:
		msg("DDL tracking : rename %u \"%.*s\",\"%.*s\"",
			space_id, int(len), name, int(new_len), new_name);
		fail = !check_if_skip_table(
				filename_to_spacename(name, len).c_str())
		       || !check_if_skip_table(
				filename_to_spacename(new_name, new_len).c_str());
		break;
	case FILE_DELETE:
		fail = !check_if_skip_table(
				filename_to_spacename(name, len).c_str());
		msg("DDL tracking : delete %u \"%.*s\"", space_id, int(len), name);
		break;
	default:
		ut_ad(0);
		break;
	}

	if (fail) {
		ut_a(opt_no_lock);
		die("DDL operation detected in the late phase of backup."
			"Backup is inconsistent. Remove --no-lock option to fix.");
	}
}

/* Function to store the space id of page0 INIT_PAGE
@param	space_id	space id which has page0 init page */
static void backup_first_page_op(space_id_t space_id)
{
  first_page_init_ids.insert(space_id);
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
  OPT_INNODB_FLUSH_METHOD,
  OPT_INNODB_LOG_GROUP_HOME_DIR,
  OPT_INNODB_MAX_DIRTY_PAGES_PCT,
  OPT_INNODB_MAX_PURGE_LAG,
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
  OPT_XTRA_MYSQLD_ARGS,
  OPT_XB_IGNORE_INNODB_PAGE_CORRUPTION,
  OPT_INNODB_FORCE_RECOVERY,
  OPT_MAX_BINLOGS
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

    {"sst_max_binlogs", OPT_MAX_BINLOGS,
     "Number of recent binary logs to be included in the backup. "
     "Setting this parameter to zero normally disables transmission "
     "of binary logs to the joiner nodes during SST using Galera. "
     "But sometimes a single current binlog can still be transmitted "
     "to the joiner even with sst_max_binlogs=0, because it is "
     "required for Galera to work properly with GTIDs support.",
     (G_PTR *) &opt_max_binlogs,
     (G_PTR *) &opt_max_binlogs, 0, GET_UINT, OPT_ARG,
     UINT_MAX, 0, UINT_MAX, 0, 1, 0},

#define MYSQL_CLIENT
#include "sslopt-longopts.h"
#undef MYSQL_CLIENT
    {0, 0, 0, 0, 0, 0, GET_NO_ARG, NO_ARG, 0, 0, 0, 0, 0, 0}};

uint xb_client_options_count = array_elements(xb_client_options);

#ifndef DBUG_OFF
/** Parameters to DBUG */
static const char *dbug_option;
#endif

#ifdef HAVE_URING
extern const char *io_uring_may_be_unsafe;
bool innodb_use_native_aio_default();
#endif

struct my_option xb_server_options[] =
{
  {"datadir", 'h', "Path to the database root.", (G_PTR*) &mysql_data_home,
   (G_PTR*) &mysql_data_home, 0, GET_STR, REQUIRED_ARG, 0, 0, 0, 0, 0, 0},
  {"tmpdir", 't',
   "Path for temporary files. Several paths may be specified, separated by a "
#if defined(_WIN32)
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
   "Enable InnoDB adaptive hash index (disabled by default).",
   &btr_search_enabled,
   &btr_search_enabled,
   0, GET_BOOL, NO_ARG, 0, 0, 0, 0, 0, 0},
#endif /* BTR_CUR_HASH_ADAPT */
  {"innodb_autoextend_increment", OPT_INNODB_AUTOEXTEND_INCREMENT,
   "Data file autoextend increment in megabytes",
   (G_PTR*) &sys_tablespace_auto_extend_increment,
   (G_PTR*) &sys_tablespace_auto_extend_increment,
   0, GET_UINT, REQUIRED_ARG, 8, 1, 1000, 0, 1, 0},
  {"innodb_data_file_path", OPT_INNODB_DATA_FILE_PATH,
   "Path to individual files and their sizes.", &innobase_data_file_path,
   &innobase_data_file_path, 0, GET_STR, REQUIRED_ARG, 0, 0, 0, 0, 0, 0},
  {"innodb_data_home_dir", OPT_INNODB_DATA_HOME_DIR,
   "The common part for InnoDB table spaces.", &innobase_data_home_dir,
   &innobase_data_home_dir, 0, GET_STR, REQUIRED_ARG, 0, 0, 0, 0, 0, 0},
  {"innodb_doublewrite", OPT_INNODB_DOUBLEWRITE,
   "Enable InnoDB doublewrite buffer during --prepare.",
   (G_PTR*) &srv_use_doublewrite_buf,
   (G_PTR*) &srv_use_doublewrite_buf, 0, GET_BOOL, NO_ARG, 0, 0, 0, 0, 0, 0},
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
   (G_PTR*) &srv_file_per_table,
   (G_PTR*) &srv_file_per_table, 0, GET_BOOL, NO_ARG,
   FALSE, 0, 0, 0, 0, 0},

  {"innodb_flush_method", OPT_INNODB_FLUSH_METHOD,
   "With which method to flush data.",
   &srv_file_flush_method, &srv_file_flush_method,
   &innodb_flush_method_typelib, GET_ENUM, REQUIRED_ARG,
   IF_WIN(SRV_ALL_O_DIRECT_FSYNC, SRV_O_DIRECT), 0, 0, 0, 0, 0},

  {"innodb_log_buffer_size", OPT_INNODB_LOG_BUFFER_SIZE,
   "Redo log buffer size in bytes.",
   (G_PTR*) &log_sys.buf_size, (G_PTR*) &log_sys.buf_size, 0,
   IF_WIN(GET_ULL,GET_ULONG), REQUIRED_ARG, 2U << 20,
   2U << 20, SIZE_T_MAX, 0, 4096, 0},
  {"innodb_log_file_size", OPT_INNODB_LOG_FILE_SIZE,
   "Ignored for mysqld option compatibility",
   (G_PTR*) &srv_log_file_size, (G_PTR*) &srv_log_file_size, 0,
   GET_ULL, REQUIRED_ARG, 96 << 20, 4 << 20,
   std::numeric_limits<ulonglong>::max(), 0, 4096, 0},
  {"innodb_log_group_home_dir", OPT_INNODB_LOG_GROUP_HOME_DIR,
   "Path to InnoDB log files.", &srv_log_group_home_dir,
   &srv_log_group_home_dir, 0, GET_STR, REQUIRED_ARG, 0, 0, 0, 0, 0, 0},
  {"innodb_max_dirty_pages_pct", OPT_INNODB_MAX_DIRTY_PAGES_PCT,
   "Percentage of dirty pages allowed in bufferpool.", (G_PTR*) &srv_max_buf_pool_modified_pct,
   (G_PTR*) &srv_max_buf_pool_modified_pct, 0, GET_ULONG, REQUIRED_ARG, 90, 0, 100, 0, 0, 0},
  {"innodb_use_native_aio", OPT_INNODB_USE_NATIVE_AIO,
   "Use native AIO if supported on this platform.",
   (G_PTR*) &srv_use_native_aio,
   (G_PTR*) &srv_use_native_aio, 0, GET_BOOL, NO_ARG,
#ifdef HAVE_URING
   innodb_use_native_aio_default(),
#else
   TRUE,
#endif
   0, 0, 0, 0, 0},
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
   "FULL_CRC32, STRICT_FULL_CRC32]", &srv_checksum_algorithm,
   &srv_checksum_algorithm, &innodb_checksum_algorithm_typelib, GET_ENUM,
   REQUIRED_ARG, SRV_CHECKSUM_ALGORITHM_CRC32, 0, 0, 0, 0, 0},

  {"innodb_undo_directory", OPT_INNODB_UNDO_DIRECTORY,
   "Directory where undo tablespace files live, this path can be absolute.",
   &srv_undo_dir, &srv_undo_dir, 0, GET_STR, REQUIRED_ARG, 0, 0, 0, 0, 0,
   0},

  {"innodb_undo_tablespaces", OPT_INNODB_UNDO_TABLESPACES,
   "Number of undo tablespaces to use.",
   (G_PTR*)&srv_undo_tablespaces, (G_PTR*)&srv_undo_tablespaces,
   0, GET_UINT, REQUIRED_ARG, 0, 0, 126, 0, 1, 0},

  {"innodb_compression_level", OPT_INNODB_COMPRESSION_LEVEL,
   "Compression level used for zlib compression.",
   (G_PTR*)&page_zip_level, (G_PTR*)&page_zip_level,
   0, GET_UINT, REQUIRED_ARG, 6, 0, 9, 0, 0, 0},

  {"defaults_group", OPT_DEFAULTS_GROUP, "defaults group in config file (default \"mysqld\").",
   (G_PTR*) &defaults_group, (G_PTR*) &defaults_group,
   0, GET_STR, REQUIRED_ARG, 0, 0, 0, 0, 0, 0},

  {"plugin-dir", OPT_PLUGIN_DIR,
  "Server plugin directory. Used to load plugins during 'prepare' phase."
  "Has no effect in the 'backup' phase (plugin directory during backup is the same as server's)",
  &xb_plugin_dir, &xb_plugin_dir,
  0, GET_STR, REQUIRED_ARG, 0, 0, 0, 0, 0, 0 },

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

    {"mysqld-args", OPT_XTRA_MYSQLD_ARGS,
     "All arguments that follow this argument are considered as server "
     "options, and if some of them are not supported by mariabackup, they "
     "will be ignored.",
     (G_PTR *) &xtrabackup_mysqld_args, (G_PTR *) &xtrabackup_mysqld_args, 0,
     GET_BOOL, NO_ARG, 0, 0, 0, 0, 0, 0},

    {"help", '?',
     "Display this help and exit.",
     (G_PTR *) &xtrabackup_help, (G_PTR *) &xtrabackup_help, 0,
     GET_BOOL, NO_ARG, 0, 0, 0, 0, 0, 0},

  { 0, 0, 0, 0, 0, 0, GET_NO_ARG, NO_ARG, 0, 0, 0, 0, 0, 0}
};

uint xb_server_options_count = array_elements(xb_server_options);


static std::set<std::string> tables_for_export;

static void append_export_table(const char *dbname, const char *tablename,
                                bool is_remote, bool skip_node_page0,
                                uint32_t defer_space_id)
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
  while (fgets(outline, FN_REFLEN - 1, outf))
    fprintf(stderr,"%s",outline);

  err = pclose(outf);
end:
  unlink(BOOTSTRAP_FILENAME);
  return err;
}

static const char *xb_client_default_groups[]= {
    "client", "client-server", "client-mariadb", 0, 0, 0};

static const char *backup_default_groups[]= {
    "xtrabackup", "mariabackup", "mariadb-backup", 0, 0, 0};

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
  print_defaults("my", load_default_groups);
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
xb_get_one_option(const struct my_option *opt,
		  const char *argument, const char *)
{
  switch(opt->id) {
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
    ut_a(srv_file_flush_method
	 <= IF_WIN(SRV_ALL_O_DIRECT_FSYNC, SRV_O_DIRECT_NO_FSYNC));
    ADD_PRINT_PARAM_OPT(innodb_flush_method_names[srv_file_flush_method]);
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

    ut_a(srv_checksum_algorithm <= SRV_CHECKSUM_ALGORITHM_STRICT_FULL_CRC32);

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
    opt_password = argument;
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

static bool innodb_init_param()
{
	srv_is_being_started = TRUE;
	/* === some variables from mysqld === */
	memset((G_PTR) &mysql_tmpdir_list, 0, sizeof(mysql_tmpdir_list));

	if (init_tmpdir(&mysql_tmpdir_list, opt_mysql_tmpdir)) {
		msg("init_tmpdir() failed");
		return true;
	}
	xtrabackup_tmpdir = my_tmpdir(&mysql_tmpdir_list);
	/* dummy for initialize all_charsets[] */
	get_charset_name(0);

	srv_page_size = 0;
	srv_page_size_shift = 0;
#ifdef BTR_CUR_HASH_ADAPT
	btr_ahi_parts = 1;
#endif /* BTR_CUR_HASH_ADAPT */

	if (innobase_page_size != (1LL << 14)) {
		size_t n_shift = get_bit_shift(size_t(innobase_page_size));

		if (n_shift >= 12 && n_shift <= UNIV_PAGE_SIZE_SHIFT_MAX) {
			srv_page_size_shift = uint32_t(n_shift);
			srv_page_size = 1U << n_shift;
			msg("InnoDB: The universal page size of the "
			    "database is set to %lu.", srv_page_size);
		} else {
			msg("invalid value of "
			    "innobase_page_size: %lld", innobase_page_size);
			goto error;
		}
	} else {
		srv_page_size_shift = 14;
		srv_page_size = 1U << 14;
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

	srv_sys_space.set_space_id(TRX_SYS_SPACE);
	srv_sys_space.set_path(srv_data_home);
	switch (srv_checksum_algorithm) {
	case SRV_CHECKSUM_ALGORITHM_FULL_CRC32:
	case SRV_CHECKSUM_ALGORITHM_STRICT_FULL_CRC32:
		srv_sys_space.set_flags(FSP_FLAGS_FCRC32_MASK_MARKER
					| FSP_FLAGS_FCRC32_PAGE_SSIZE());
		break;
	default:
		srv_sys_space.set_flags(FSP_FLAGS_PAGE_SSIZE());
	}

	if (!srv_sys_space.parse_params(innobase_data_file_path, true)) {
		goto error;
	}

	srv_sys_space.normalize_size();
	srv_lock_table_size = 5 * (srv_buf_pool_size >> srv_page_size_shift);

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

	if (strchr(srv_log_group_home_dir, ';')) {
		msg("syntax error in innodb_log_group_home_dir, ");
		goto error;
	}

	srv_adaptive_flushing = FALSE;

        /* We set srv_pool_size here in units of 1 kB. InnoDB internally
        changes the value so that it becomes the number of database pages. */

	srv_buf_pool_size = (ulint) xtrabackup_use_memory;
	srv_buf_pool_chunk_unit = srv_buf_pool_size;

	srv_n_file_io_threads = (uint) innobase_file_io_threads;
	srv_n_read_io_threads = (uint) innobase_read_io_threads;
	srv_n_write_io_threads = (uint) innobase_write_io_threads;

	srv_max_n_open_files = ULINT_UNDEFINED - 5;

	srv_print_verbose_log = verbose ? 2 : 1;

	/* Store the default charset-collation number of this MySQL
	installation */

	/* We cannot treat characterset here for now!! */
	data_mysql_default_charset_coll = (ulint)default_charset_info->number;

	ut_ad(DATA_MYSQL_BINARY_CHARSET_COLL == my_charset_bin.number);

#ifdef _WIN32
	srv_use_native_aio = TRUE;

#elif defined(LINUX_NATIVE_AIO)

	if (srv_use_native_aio) {
		msg("InnoDB: Using Linux native AIO");
	}
#elif defined(HAVE_URING)
	if (!srv_use_native_aio) {
	} else if (io_uring_may_be_unsafe) {
		msg("InnoDB: Using liburing on this kernel %s may cause hangs;"
		    " see https://jira.mariadb.org/browse/MDEV-26674",
		    io_uring_may_be_unsafe);
	} else {
		msg("InnoDB: Using liburing");
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

#ifdef _WIN32
	srv_use_native_aio = TRUE;
#endif
	return false;

error:
	msg("mariabackup: innodb_init_param(): Error occurred.\n");
	return true;
}

static byte log_hdr_buf[log_t::START_OFFSET + SIZE_OF_FILE_CHECKPOINT];

/** Initialize an InnoDB log file header in log_hdr_buf[] */
static void log_hdr_init()
{
  memset(log_hdr_buf, 0, sizeof log_hdr_buf);
  mach_write_to_4(LOG_HEADER_FORMAT + log_hdr_buf, log_t::FORMAT_10_8);
  mach_write_to_8(LOG_HEADER_START_LSN + log_hdr_buf,
                  log_sys.next_checkpoint_lsn);
  snprintf(reinterpret_cast<char*>(LOG_HEADER_CREATOR + log_hdr_buf),
           16, "Backup %u.%u.%u",
           MYSQL_VERSION_ID / 10000, MYSQL_VERSION_ID / 100 % 100,
           MYSQL_VERSION_ID % 100);
  if (log_sys.is_encrypted())
    log_crypt_write_header(log_hdr_buf + LOG_HEADER_CREATOR_END);
  mach_write_to_4(508 + log_hdr_buf, my_crc32c(0, log_hdr_buf, 508));
  mach_write_to_8(log_hdr_buf + 0x1000, log_sys.next_checkpoint_lsn);
  mach_write_to_8(log_hdr_buf + 0x1008, recv_sys.lsn);
  mach_write_to_4(log_hdr_buf + 0x103c,
                  my_crc32c(0, log_hdr_buf + 0x1000, 60));
}

static bool innodb_init()
{
  bool create_new_db= false;

  srv_max_io_capacity= srv_io_capacity >= SRV_MAX_IO_CAPACITY_LIMIT / 2
    ? SRV_MAX_IO_CAPACITY_LIMIT : std::max(2 * srv_io_capacity, 2000UL);

  /* Check if the data files exist or not. */
  dberr_t err= srv_sys_space.check_file_spec(&create_new_db, 5U << 20);

  if (err == DB_SUCCESS)
    err= srv_start(create_new_db);

  if (err != DB_SUCCESS)
  {
    msg("mariadb-backup: srv_start() returned %d (%s).", err, ut_strerr(err));
    return true;
  }

  ut_ad(srv_force_recovery <= SRV_FORCE_IGNORE_CORRUPT);
  ut_ad(recv_no_log_write);
  buf_flush_sync();
  DBUG_ASSERT(!buf_pool.any_io_pending());
  log_sys.close_file();

  if (xtrabackup_incremental)
    /* Reset the ib_logfile0 in --target-dir, not --incremental-dir. */
    srv_log_group_home_dir= xtrabackup_target_dir;

  bool ret;
  const std::string ib_logfile0{get_log_file_path()};
  os_file_delete_if_exists_func(ib_logfile0.c_str(), nullptr);
  os_file_t file= os_file_create_func(ib_logfile0.c_str(),
                                      OS_FILE_CREATE, OS_FILE_NORMAL,
                                      OS_DATA_FILE_NO_O_DIRECT, false, &ret);
  if (!ret)
  {
  invalid_log:
    msg("mariadb-backup: Cannot create %s", ib_logfile0.c_str());
    return true;
  }

  recv_sys.lsn= log_sys.next_checkpoint_lsn=
    log_sys.get_lsn() - SIZE_OF_FILE_CHECKPOINT;
  log_sys.set_latest_format(false); // not encrypted
  log_hdr_init();
  byte *b= &log_hdr_buf[log_t::START_OFFSET];
  b[0]= FILE_CHECKPOINT | 10;
  mach_write_to_8(b + 3, recv_sys.lsn);
  b[11]= 1;
  mach_write_to_4(b + 12, my_crc32c(0, b, 11));
  static_assert(12 + 4 == SIZE_OF_FILE_CHECKPOINT, "compatibility");

#ifdef _WIN32
  DWORD len;
  ret= WriteFile(file, log_hdr_buf, sizeof log_hdr_buf,
                 &len, nullptr) && len == sizeof log_hdr_buf;
#else
  ret= sizeof log_hdr_buf == write(file, log_hdr_buf, sizeof log_hdr_buf);
#endif
  if (!os_file_close_func(file) || !ret)
    goto invalid_log;
  return false;
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
	info->space_id = UINT32_MAX;

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
				info->space_id = static_cast<uint32_t>
					(strtoul(value, NULL, 10));
			}
		}
	}

	fclose(fp);

	if (page_size == ULINT_UNDEFINED) {
		msg("page_size is required in %s", filepath);
		r = FALSE;
	} else {
		info->page_size = zip_size ? zip_size : page_size;
	}

	if (info->space_id == UINT32_MAX) {
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
		 "space_id = %u\n",
		 info->page_size,
		 info->zip_size,
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
void xtrabackup_io_throttling()
{
  if (!xtrabackup_backup)
    return;

  mysql_mutex_lock(&recv_sys.mutex);
  if (xtrabackup_throttle && (io_ticket--) < 0)
    mysql_cond_wait(&wait_throttle, &recv_sys.mutex);
  mysql_mutex_unlock(&recv_sys.mutex);
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
	const ulint fold = my_crc32c(0, name, strlen(name));
	HASH_SEARCH(name_hash, table, fold,
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
	if (regex_list.empty() && !tables_hash->array) {
		return(FALSE);
	}

	if (regex_list_check_match(regex_list, name)) {
		return(TRUE);
	}

	return tables_hash->array &&
		find_filter_in_hashtable(name, tables_hash, NULL);
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

	if (databases_exclude_hash.array &&
		find_filter_in_hashtable(name, &databases_exclude_hash,
					 &database) &&
		(!database->has_tables || !databases_include_hash.array)) {
		/* Database is found and there are no tables specified,
		   skip entire db. */
		return DATABASE_SKIP;
	}

	if (databases_include_hash.array) {
		if (!find_filter_in_hashtable(name, &databases_include_hash,
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
	if (!databases_include_hash.array && !databases_exclude_hash.array) {
		return(FALSE);
	}

	const char* db_name = strrchr(path, '/');
#ifdef _WIN32
	if (const char* last = strrchr(path, '\\')) {
		if (!db_name || last > db_name) {
			db_name = last;
		}
	}
#endif

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


	dbname = NULL;
	tbname = name;
	for (;;) {
		ptr= strchr(tbname, '/');
#ifdef _WIN32
		if (!ptr) {
			ptr= strchr(tbname,'\\');
		}
#endif
		if (!ptr) {
			break;
		}
		dbname = tbname;
		tbname = ptr + 1;
	}

	if (strncmp(tbname, tmp_file_prefix, tmp_file_prefix_length) == 0) {
		return TRUE;
	}

	if (regex_exclude_list.empty() &&
		regex_include_list.empty() &&
		!tables_include_hash.array &&
		!tables_exclude_hash.array &&
		!databases_include_hash.array &&
		!databases_exclude_hash.array) {
		return(FALSE);
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
					   &tables_exclude_hash)) {
		return(TRUE);
	}
	if (check_if_table_matches_filters(buf, regex_include_list,
					   &tables_include_hash)) {
		return(FALSE);
	}
	if ((eptr = strstr(buf, "#P#")) != NULL) {
		*eptr = 0;

		if (check_if_table_matches_filters(buf, regex_exclude_list,
						   &tables_exclude_hash)) {
			return (TRUE);
		}
		if (check_if_table_matches_filters(buf, regex_include_list,
						   &tables_include_hash)) {
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
		tables_include_hash.array) {

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

	if (fil_is_user_tablespace_id(node->space->id)
	    && check_if_skip_table(filename_to_spacename(node->name,
							 strlen(node->name)).
				   c_str())) {
		msg(thread_n, "Skipping %s.", node->name);
		return(FALSE);
	}

	memset(&write_filt_ctxt, 0, sizeof(xb_write_filt_ctxt_t));

	bool was_dropped;
	mysql_mutex_lock(&recv_sys.mutex);
	was_dropped = (ddl_tracker.drops.find(node->space->id) != ddl_tracker.drops.end());
	mysql_mutex_unlock(&recv_sys.mutex);
	if (was_dropped) {
		if (node->is_open()) {
			mysql_mutex_lock(&fil_system.mutex);
			node->close();
			mysql_mutex_unlock(&fil_system.mutex);
		}
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
		msg(thread_n, "%s %s", action, node->name);
	} else {
		msg(thread_n, "%s %s to %s", action, node->name,
		    dstfile->path);
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
	} else {
		const fil_space_t::name_type name = node->space->name();

		mysql_mutex_lock(&recv_sys.mutex);
		ddl_tracker.tables_in_backup.emplace(node->space->id,
						     std::string(name.data(),
								 name.size()));
		mysql_mutex_unlock(&recv_sys.mutex);
	}

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
	msg(thread_n,"Warning: We assume the  table was dropped during xtrabackup execution and ignore the tablespace %s", node->name);
	return(FALSE);
}

/** Copy redo log until the current end of the log is reached
@return	whether the operation failed */
static bool xtrabackup_copy_logfile()
{
  mysql_mutex_assert_owner(&recv_sys.mutex);
  DBUG_EXECUTE_IF("log_checksum_mismatch", return false;);

  ut_a(dst_log_file);
  ut_ad(recv_sys.is_initialised());
  const size_t sequence_offset{log_sys.is_encrypted() ? 8U + 5U : 5U};
  const size_t block_size_1{log_sys.get_block_size() - 1};

#ifdef HAVE_PMEM
  if (log_sys.is_pmem())
  {
    recv_sys.offset= size_t(log_sys.calc_lsn_offset(recv_sys.lsn));
    recv_sys.len= size_t(log_sys.file_size);
  }
  else
#endif
  {
    recv_sys.offset= size_t(recv_sys.lsn - log_sys.get_first_lsn()) &
      block_size_1;
    recv_sys.len= 0;
  }

  for (unsigned retry_count{0};;)
  {
    recv_sys_t::parse_mtr_result r;
    size_t start_offset{recv_sys.offset};

#ifdef HAVE_PMEM
    if (log_sys.is_pmem())
    {
      if ((ut_d(r=) recv_sys.parse_pmem(STORE_NO)) != recv_sys_t::OK)
      {
        ut_ad(r == recv_sys_t::GOT_EOF);
        goto retry;
      }

      retry_count= 0;

      do
      {
        const byte seq{log_sys.get_sequence_bit(recv_sys.lsn -
                                                sequence_offset)};
        ut_ad(recv_sys.offset >= log_sys.START_OFFSET);
        ut_ad(recv_sys.offset < recv_sys.len);
        ut_ad(log_sys.buf[recv_sys.offset
                          >= log_sys.START_OFFSET + sequence_offset
                          ? recv_sys.offset - sequence_offset
                          : recv_sys.len - sequence_offset +
                          recv_sys.offset - log_sys.START_OFFSET] ==
              seq);
        static const byte seq_1{1};
        if (UNIV_UNLIKELY(start_offset > recv_sys.offset))
        {
          const ssize_t so(recv_sys.offset - (log_sys.START_OFFSET +
                                              sequence_offset));
          if (so <= 0)
          {
            if (ds_write(dst_log_file, log_sys.buf + start_offset,
                         recv_sys.len - start_offset + so) ||
                ds_write(dst_log_file, &seq_1, 1))
              goto write_error;
            if (so < -1 &&
                ds_write(dst_log_file, log_sys.buf + recv_sys.len + (1 + so),
                         -(1 + so)))
              goto write_error;
            if (ds_write(dst_log_file, log_sys.buf + log_sys.START_OFFSET,
                         recv_sys.offset - log_sys.START_OFFSET))
              goto write_error;
          }
          else
          {
            if (ds_write(dst_log_file, log_sys.buf + start_offset,
                         recv_sys.len - start_offset))
              goto write_error;
            if (ds_write(dst_log_file, log_sys.buf + log_sys.START_OFFSET, so))
              goto write_error;
            if (ds_write(dst_log_file, &seq_1, 1))
              goto write_error;
            if (so > 1 &&
                ds_write(dst_log_file, log_sys.buf + recv_sys.offset -
                         (so - 1), so - 1))
              goto write_error;
          }
        }
        else if (seq == 1)
        {
          if (ds_write(dst_log_file, log_sys.buf + start_offset,
                       recv_sys.offset - start_offset))
            goto write_error;
        }
        else if (ds_write(dst_log_file, log_sys.buf + start_offset,
                          recv_sys.offset - start_offset - sequence_offset) ||
                 ds_write(dst_log_file, &seq_1, 1) ||
                 ds_write(dst_log_file, log_sys.buf +
                          recv_sys.offset - sequence_offset + 1,
                          sequence_offset - 1))
          goto write_error;

        start_offset= recv_sys.offset;
      }
      while ((ut_d(r=)recv_sys.parse_pmem(STORE_NO)) == recv_sys_t::OK);

      ut_ad(r == recv_sys_t::GOT_EOF);
      pthread_cond_broadcast(&scanned_lsn_cond);
      break;
    }
    else
#endif
    {
      {
        auto source_offset=
          log_sys.calc_lsn_offset(recv_sys.lsn + recv_sys.len -
                                  recv_sys.offset);
        source_offset&= ~block_size_1;
        size_t size{log_sys.buf_size - recv_sys.len};
        if (UNIV_UNLIKELY(source_offset + size > log_sys.file_size))
        {
          const size_t first{size_t(log_sys.file_size - source_offset)};
          ut_ad(first <= log_sys.buf_size);
          log_sys.log.read(source_offset, {log_sys.buf, first});
          size-= first;
          if (log_sys.START_OFFSET + size > source_offset)
            size= size_t(source_offset - log_sys.START_OFFSET);
          if (size)
            log_sys.log.read(log_sys.START_OFFSET,
                             {log_sys.buf + first, size});
          size+= first;
        }
        else
          log_sys.log.read(source_offset, {log_sys.buf, size});
        recv_sys.len= size;
      }

      if (log_sys.buf[recv_sys.offset] <= 1)
        break;

      if (recv_sys.parse_mtr(STORE_NO) == recv_sys_t::OK)
      {
        do
        {
          /* Set the sequence bit (the backed-up log will not wrap around) */
          byte *seq= &log_sys.buf[recv_sys.offset - sequence_offset];
          ut_ad(*seq == log_sys.get_sequence_bit(recv_sys.lsn -
                                                 sequence_offset));
          *seq= 1;
        }
        while ((r= recv_sys.parse_mtr(STORE_NO)) == recv_sys_t::OK);

        if (ds_write(dst_log_file, log_sys.buf + start_offset,
                     recv_sys.offset - start_offset))
        {
#ifdef HAVE_PMEM
        write_error:
#endif
          msg("Error: write to ib_logfile0 failed");
          return true;
        }
        else
        {
          const auto ofs= recv_sys.offset & ~block_size_1;
          memmove_aligned<64>(log_sys.buf, log_sys.buf + ofs,
                              recv_sys.len - ofs);
          recv_sys.len-= ofs;
          recv_sys.offset&= block_size_1;
        }

        pthread_cond_broadcast(&scanned_lsn_cond);

        if (r == recv_sys_t::GOT_EOF)
          break;

        if (recv_sys.offset < log_sys.get_block_size())
          break;

        if (xtrabackup_throttle && io_ticket-- < 0)
          mysql_cond_wait(&wait_throttle, &recv_sys.mutex);

        retry_count= 0;
        continue;
      }
      else
      {
        recv_sys.len= recv_sys.offset & ~block_size_1;
#ifdef HAVE_PMEM
      retry:
#endif
        if (retry_count == 100)
          break;

        mysql_mutex_unlock(&recv_sys.mutex);
        if (!retry_count++)
          msg("Retrying read of log at LSN=" LSN_PF, recv_sys.lsn);
        my_sleep(1000);
      }
    }
    mysql_mutex_lock(&recv_sys.mutex);
  }

  msg(">> log scanned up to (" LSN_PF ")", recv_sys.lsn);
  return false;
}

/**
Wait until redo log copying thread processes given lsn
*/
void backup_wait_for_lsn(lsn_t lsn)
{
  mysql_mutex_lock(&recv_sys.mutex);
  for (lsn_t last_lsn{recv_sys.lsn}; last_lsn < lsn; )
  {
    timespec abstime;
    set_timespec(abstime, 5);
    if (my_cond_timedwait(&scanned_lsn_cond, &recv_sys.mutex.m_mutex,
                          &abstime) &&
        last_lsn == recv_sys.lsn)
      die("Was only able to copy log from " LSN_PF " to " LSN_PF
          ", not " LSN_PF "; try increasing innodb_log_file_size",
          log_sys.next_checkpoint_lsn, last_lsn, lsn);
    last_lsn= recv_sys.lsn;
  }
  mysql_mutex_unlock(&recv_sys.mutex);
}

extern lsn_t server_lsn_after_lock;

static void log_copying_thread()
{
  my_thread_init();
  mysql_mutex_lock(&recv_sys.mutex);
  while (!xtrabackup_copy_logfile() &&
         (!metadata_to_lsn || metadata_to_lsn > recv_sys.lsn))
  {
    timespec abstime;
    set_timespec_nsec(abstime, 1000000ULL * xtrabackup_log_copy_interval);
    mysql_cond_timedwait(&log_copying_stop, &recv_sys.mutex, &abstime);
  }
  log_copying_running= false;
  mysql_mutex_unlock(&recv_sys.mutex);
  my_thread_end();
}

static bool have_io_watching_thread;
static pthread_t io_watching_thread_id;

/* io throttle watching (rough) */
static void *io_watching_thread(void*)
{
  /* currently, for --backup only */
  ut_a(xtrabackup_backup);

  mysql_mutex_lock(&recv_sys.mutex);

  while (log_copying_running && !metadata_to_lsn)
  {
    timespec abstime;
    set_timespec(abstime, 1);
    mysql_cond_timedwait(&log_copying_stop, &recv_sys.mutex, &abstime);
    io_ticket= xtrabackup_throttle;
    mysql_cond_broadcast(&wait_throttle);
  }

  /* stop io throttle */
  xtrabackup_throttle= 0;
  mysql_cond_broadcast(&wait_throttle);
  mysql_mutex_unlock(&recv_sys.mutex);
  return nullptr;
}

#ifndef DBUG_OFF
char *dbug_mariabackup_get_val(const char *event,
                               const fil_space_t::name_type key)
{
  char envvar[FN_REFLEN];
  strncpy(envvar, event, sizeof envvar - 1);
  envvar[(sizeof envvar) - 1] = '\0';

  if (key.size() && key.size() + strlen(envvar) < (sizeof envvar) - 2)
  {
    strcat(envvar, "_");
    strncat(envvar, key.data(), key.size());
    if (char *slash= strchr(envvar, '/'))
      *slash= '_';
  }

  char *val = getenv(envvar);
  return val && *val ? val : nullptr;
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
void dbug_mariabackup_event(const char *event,
                                   const fil_space_t::name_type key)
{
	char *sql = dbug_mariabackup_get_val(event, key);
	if (sql && *sql) {
		msg("dbug_mariabackup_event : executing '%s'", sql);
		xb_mysql_query(mysql_connection, sql, false, true);
	}
}
#endif // DBUG_OFF

/** Datafiles copying thread.*/
static void data_copy_thread_func(data_thread_ctxt_t *ctxt) /* thread context */
{
	uint			num = ctxt->num;
	fil_node_t*		node;
	ut_ad(ctxt->corrupted_pages);

	/*
	  Initialize mysys thread-specific memory so we can
	  use mysys functions in this thread.
	*/
	my_thread_init();

	while ((node = datafiles_iter_next(ctxt->it)) != NULL) {
		DBUG_MARIABACKUP_EVENT("before_copy", node->space->name());
		DBUG_EXECUTE_FOR_KEY("wait_innodb_redo_before_copy",
				     node->space->name(),
			backup_wait_for_lsn(get_current_lsn(mysql_connection)););
		/* copy the datafile */
		if (xtrabackup_copy_datafile(node, num, NULL,
			xtrabackup_incremental ? wf_incremental : wf_write_through,
			*ctxt->corrupted_pages))
			die("failed to copy datafile.");

		DBUG_MARIABACKUP_EVENT("after_copy", node->space->name());
	}

	pthread_mutex_lock(ctxt->count_mutex);
	(*ctxt->count)--;
	pthread_mutex_unlock(ctxt->count_mutex);

	my_thread_end();
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
	fil_system.create(srv_file_per_table ? 50000 : 5000);
	fil_system.freeze_space_list = 1;
	fil_system.space_id_reuse_warned = true;
}

/** Load tablespace.

@param[in] dirname directory name of the tablespace to open
@param[in] filname file name of the tablespece to open
@param[in] is_remote true if tablespace file is .isl
@param[in] skip_node_page0 true if we don't need to read node page 0. Otherwise
node page0 will be read, and it's size and free pages limit
will be set from page 0, what is neccessary for checking and fixing corrupted
pages.
@param[in] defer_space_id use the space id to create space object
when there is deferred tablespace
*/
static void xb_load_single_table_tablespace(const char *dirname,
                                            const char *filname,
                                            bool is_remote,
                                            bool skip_node_page0,
                                            uint32_t defer_space_id)
{
	ut_ad(srv_operation == SRV_OPERATION_BACKUP
	      || srv_operation == SRV_OPERATION_RESTORE_DELTA
	      || srv_operation == SRV_OPERATION_RESTORE
	      || srv_operation == SRV_OPERATION_BACKUP_NO_DEFER);
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
	dberr_t	err;
	fil_space_t	*space;
	bool	defer = false;

	name = static_cast<char*>(ut_malloc_nokey(pathlen));

	if (dirname != NULL) {
		snprintf(name, pathlen, "%s/%s", dirname, filname);
		name[pathlen - 5] = 0;
	} else {
		snprintf(name, pathlen, "%s", filname);
		name[pathlen - 5] = 0;
	}

	const fil_space_t::name_type n{name, pathlen - 5};
	Datafile *file;

	if (is_remote) {
		RemoteDatafile* rf = new RemoteDatafile();
		if (!rf->open_link_file(n)) {
			die("Can't open datafile %s", name);
		}
		file = rf;
	} else {
		file = new Datafile();
		file->make_filepath(".", n, IBD);
	}

	if (file->open_read_only(true) != DB_SUCCESS) {
		die("Can't open datafile %s", name);
	}

	for (int i = 0; i < 10; i++) {
		file->m_defer = false;
		err = file->validate_first_page();

		if (file->m_defer) {
			if (defer_space_id) {
				defer = true;
				file->set_space_id(defer_space_id);
				file->set_flags(FSP_FLAGS_PAGE_SSIZE());
				err = DB_SUCCESS;
				break;
			}
		} else if (err != DB_CORRUPTION) {
			break;
		}

		my_sleep(1000);
	}

	if (!defer && file->m_defer) {
		const char *file_path = file->filepath();
		defer_space_names.insert(
			filename_to_spacename(
				file_path, strlen(file_path)));
		delete file;
		ut_free(name);
		return;
	}

	bool is_empty_file = file->exists() && file->is_empty_file();

	if (err == DB_SUCCESS && file->space_id() != SRV_TMP_SPACE_ID) {
		space = fil_space_t::create(
			file->space_id(), file->flags(),
			FIL_TYPE_TABLESPACE, NULL/* TODO: crypt_data */);

		ut_a(space != NULL);
		fil_node_t* node= space->add(
			file->filepath(),
			skip_node_page0 ? file->detach() : pfs_os_file_t(),
			0, false, false);
		node->deferred= defer;
		mysql_mutex_lock(&fil_system.mutex);
		if (!space->read_page0())
                  err= DB_CANNOT_OPEN_FILE;
		mysql_mutex_unlock(&fil_system.mutex);

		if (srv_operation == SRV_OPERATION_RESTORE_DELTA
		    || xb_close_files) {
			space->close();
		}
	}

	delete file;

	if (err != DB_SUCCESS && xtrabackup_backup && !is_empty_file) {
		die("Failed to validate first page of the file %s, error %d",name, (int)err);
	}

	ut_free(name);
}

static void xb_load_single_table_tablespace(const std::string &space_name,
                                            bool skip_node_page0,
                                            uint32_t defer_space_id)
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
  if (!p)
    die("Unexpected tablespace %s filename %s", space_name.c_str(),
        name.c_str());
  *p= 0;
  const char *tablename= p + 1;
  xb_load_single_table_tablespace(dbname, tablename, is_remote,
                                  skip_node_page0, defer_space_id);
}

#ifdef _WIN32
/**
The os_file_opendir() function opens a directory stream corresponding to the
directory named by the dirname argument. The directory stream is positioned
at the first entry. In both Unix and Windows we automatically skip the '.'
and '..' items at the start of the directory listing.
@param[in]	dirname		directory name; it must not contain a trailing
				'\' or '/'
@return directory stream, NULL if error */
os_file_dir_t os_file_opendir(const char *dirname)
{
  char path[OS_FILE_MAX_PATH + 3];

  ut_a(strlen(dirname) < OS_FILE_MAX_PATH);

  strcpy(path, dirname);
  strcpy(path + strlen(path), "\\*");

  /* Note that in Windows opening the 'directory stream' also retrieves
  the first entry in the directory. Since it is '.', that is no problem,
  as we will skip over the '.' and '..' entries anyway. */

  LPWIN32_FIND_DATA lpFindFileData= static_cast<LPWIN32_FIND_DATA>
    (ut_malloc_nokey(sizeof(WIN32_FIND_DATA)));
  os_file_dir_t dir= FindFirstFile((LPCTSTR) path, lpFindFileData);
  ut_free(lpFindFileData);

  return dir;
}
#endif

/** This function returns information of the next file in the directory. We jump
over the '.' and '..' entries in the directory.
@param[in]	dirname		directory name or path
@param[in]	dir		directory stream
@param[out]	info		buffer where the info is returned
@return 0 if ok, -1 if error, 1 if at the end of the directory */
int
os_file_readdir_next_file(
	const char*	dirname,
	os_file_dir_t	dir,
	os_file_stat_t* info)
{
#ifdef _WIN32
	BOOL		ret;
	int		status;
	WIN32_FIND_DATA find_data;

next_file:
	ret = FindNextFile(dir, &find_data);

	if (ret > 0) {

		const char* name;

		name = static_cast<const char*>(find_data.cFileName);

		ut_a(strlen(name) < OS_FILE_MAX_PATH);

		if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) {

			goto next_file;
		}

		strcpy(info->name, name);

		info->size = find_data.nFileSizeHigh;
		info->size <<= 32;
		info->size |= find_data.nFileSizeLow;

		if (find_data.dwFileAttributes
		    & FILE_ATTRIBUTE_REPARSE_POINT) {

			/* TODO: test Windows symlinks */
			/* TODO: MySQL has apparently its own symlink
			implementation in Windows, dbname.sym can
			redirect a database directory:
			REFMAN "windows-symbolic-links.html" */

			info->type = OS_FILE_TYPE_LINK;

		} else if (find_data.dwFileAttributes
			   & FILE_ATTRIBUTE_DIRECTORY) {

			info->type = OS_FILE_TYPE_DIR;

		} else {

			/* It is probably safest to assume that all other
			file types are normal. Better to check them rather
			than blindly skip them. */

			info->type = OS_FILE_TYPE_FILE;
		}

		status = 0;

	} else {
		DWORD err = GetLastError();
		if (err == ERROR_NO_MORE_FILES) {
			status = 1;
		} else {
			msg("FindNextFile in %s returned %lu", dirname, err);
			status = -1;
		}
	}

	return(status);
#else
	struct dirent*	ent;
	char*		full_path;
	int		ret;
	struct stat	statinfo;

next_file:

	ent = readdir(dir);

	if (ent == NULL) {

		return(1);
	}

	ut_a(strlen(ent->d_name) < OS_FILE_MAX_PATH);

	if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) {

		goto next_file;
	}

	strcpy(info->name, ent->d_name);

	full_path = static_cast<char*>(
		ut_malloc_nokey(strlen(dirname) + strlen(ent->d_name) + 10));
	if (!full_path) {
		return -1;
	}

	sprintf(full_path, "%s/%s", dirname, ent->d_name);

	ret = stat(full_path, &statinfo);

	if (ret) {

		if (errno == ENOENT) {
			/* readdir() returned a file that does not exist,
			it must have been deleted in the meantime. Do what
			would have happened if the file was deleted before
			readdir() - ignore and go to the next entry.
			If this is the last entry then info->name will still
			contain the name of the deleted file when this
			function returns, but this is not an issue since the
			caller shouldn't be looking at info when end of
			directory is returned. */

			ut_free(full_path);

			goto next_file;
		}

		msg("stat %s: Got error %d", full_path, errno);

		ut_free(full_path);

		return(-1);
	}

	info->size = statinfo.st_size;

	if (S_ISDIR(statinfo.st_mode)) {
		info->type = OS_FILE_TYPE_DIR;
	} else if (S_ISLNK(statinfo.st_mode)) {
		info->type = OS_FILE_TYPE_LINK;
	} else if (S_ISREG(statinfo.st_mode)) {
		info->type = OS_FILE_TYPE_FILE;
	} else {
		info->type = OS_FILE_TYPE_UNKNOWN;
	}

	ut_free(full_path);
	return(0);
#endif
}

/***********************************************************************//**
A fault-tolerant function that tries to read the next file name in the
directory. We retry 100 times if os_file_readdir_next_file() returns -1. The
idea is to read as much good data as we can and jump over bad data.
@return 0 if ok, -1 if error even after the retries, 1 if at the end
of the directory */
int
fil_file_readdir_next_file(
/*=======================*/
	dberr_t*	err,	/*!< out: this is set to DB_ERROR if an error
				was encountered, otherwise not changed */
	const char*	dirname,/*!< in: directory name or path */
	os_file_dir_t	dir,	/*!< in: directory stream */
	os_file_stat_t* info)	/*!< in/out: buffer where the
				info is returned */
{
	for (ulint i = 0; i < 100; i++) {
		int	ret = os_file_readdir_next_file(dirname, dir, info);

		if (ret != -1) {

			return(ret);
		}

		ib::error() << "os_file_readdir_next_file() returned -1 in"
			" directory " << dirname
			<< ", crash recovery may have failed"
			" for some .ibd files!";

		*err = DB_ERROR;
	}

	return(-1);
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

	dir = os_file_opendir(fil_path_to_mysql_datadir);

	if (UNIV_UNLIKELY(dir == IF_WIN(INVALID_HANDLE_VALUE, nullptr))) {
		msg("cannot open dir %s", fil_path_to_mysql_datadir);
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
		if (dbinfo.type != OS_FILE_TYPE_FILE) {
			const bool is_isl = ends_with(dbinfo.name, ".isl");
			if (is_isl || ends_with(dbinfo.name,".ibd")) {
				(*callback)(nullptr, dbinfo.name, is_isl,
					    false, 0);
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

		if (check_if_skip_database_by_path(dbpath)) {
			fprintf(stderr, "Skipping db: %s\n", dbpath);
			goto next_datadir_item;
		}

		dbdir = os_file_opendir(dbpath);

		if (UNIV_UNLIKELY(dbdir != IF_WIN(INVALID_HANDLE_VALUE,NULL))){
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
						(*callback)(dbinfo.name, fileinfo.name, is_isl, false, 0);
				}
			}

			if (os_file_closedir_failed(dbdir)) {
				fprintf(stderr, "InnoDB: Warning: could not"
				 " close database directory %s\n",
					dbpath);

				err = DB_ERROR;
			}

		} else {
			msg("Can't open dir %s", dbpath);
			err = DB_ERROR;
			break;

		}

next_datadir_item:
		ret = fil_file_readdir_next_file(&err,
						 fil_path_to_mysql_datadir,
						 dir, &dbinfo);
	}

	ut_free(dbpath);

	if (os_file_closedir_failed(dir)) {
		fprintf(stderr,
			"InnoDB: Error: could not close MariaDB datadir\n");
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
	bool		ret;
	dberr_t		error = DB_SUCCESS;
	uint32_t	space;
	uint32_t 	fsp_flags;
	int		n_retries = 5;

	if (srv_undo_tablespaces == 0) {
		return error;
	}

	file = os_file_create(0, srv_sys_space.first_datafile()->filepath(),
		OS_FILE_OPEN, OS_FILE_NORMAL, OS_DATA_FILE, true, &ret);

	if (!ret) {
		msg("Error opening %s", srv_sys_space.first_datafile()->filepath());
		return DB_ERROR;
	}

	byte* page = static_cast<byte*>
		(aligned_malloc(srv_page_size, srv_page_size));

	if (os_file_read(IORequestRead, file, page, 0, srv_page_size)
	    != DB_SUCCESS) {
		msg("Reading first page failed.\n");
		error = DB_ERROR;
		goto func_exit;
	}

	fsp_flags = mach_read_from_4(
		page + FSP_HEADER_OFFSET + FSP_SPACE_FLAGS);
retry:
	if (os_file_read(IORequestRead, file, page,
			 TRX_SYS_PAGE_NO << srv_page_size_shift,
			 srv_page_size) != DB_SUCCESS) {
		msg("Reading TRX_SYS page failed.");
		error = DB_ERROR;
		goto func_exit;
	}

	/* TRX_SYS page can't be compressed or encrypted. */
	if (buf_page_is_corrupted(false, page, fsp_flags)) {
		if (n_retries--) {
			std::this_thread::sleep_for(
				std::chrono::milliseconds(1));
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

	space = mach_read_from_4(TRX_SYS + TRX_SYS_RSEGS
				 + TRX_SYS_RSEG_SLOT_SIZE
				 + TRX_SYS_RSEG_SPACE + page);

	srv_undo_space_id_start = space;

func_exit:
	aligned_free(page);
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

	ut_ad(srv_operation == SRV_OPERATION_BACKUP
	      || srv_operation == SRV_OPERATION_RESTORE_DELTA);

	err = srv_sys_space.check_file_spec(&create_new_db, 0);

	/* create_new_db must not be true. */
	if (err != DB_SUCCESS || create_new_db) {
		msg("Could not find data files at the specified datadir");
		return(DB_ERROR);
	}

	for (int i= 0; i < 10; i++) {
		err = srv_sys_space.open_or_create(false, false, &sum_of_new_sizes);
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

	DBUG_MARIABACKUP_EVENT("after_load_tablespaces", {});
	return(DB_SUCCESS);
}

/** Destroy the tablespace memory cache. */
static void xb_data_files_close()
{
  fil_space_t::close_all();
  buf_dblwr.close();
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
	const char*	name,	/*!< in: name of table/database */
	hash_table_t*	hash)	/*!< in/out: hash to insert into */
{
	xb_filter_entry_t* entry = xb_new_filter_entry(name);

	if (UNIV_UNLIKELY(!hash->array)) {
		hash->create(1000);
	}
	const ulint fold = my_crc32c(0, entry->name, strlen(entry->name));
	HASH_INSERT(xb_filter_entry_t, name_hash, hash, fold, entry);
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
	hash_table_t* databases_hash,
	hash_table_t* tables_hash
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

		if (databases_hash && databases_hash->array) {
			const ulint fold = my_crc32c(0, dbname, p - name);
			HASH_SEARCH(name_hash, databases_hash,
					fold,
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
	for (i = 0; i < hash->n_cells; i++) {
		xb_filter_entry_t*	table;

		table = static_cast<xb_filter_entry_t *>
			(HASH_GET_FIRST(hash, i));

		while (table) {
			xb_filter_entry_t*	prev_table = table;

			table = static_cast<xb_filter_entry_t *>
				(HASH_GET_NEXT(name_hash, prev_table));
			const ulint fold = my_crc32c(0, prev_table->name,
						     strlen(prev_table->name));
			HASH_DELETE(xb_filter_entry_t, name_hash, hash,
				    fold, prev_table);
			free(prev_table);
		}
	}

	hash->free();
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

	if (tables_include_hash.array) {
		xb_filter_hash_free(&tables_include_hash);
	}

	if (tables_exclude_hash.array) {
		xb_filter_hash_free(&tables_exclude_hash);
	}

	if (databases_include_hash.array) {
		xb_filter_hash_free(&databases_include_hash);
	}

	if (databases_exclude_hash.array) {
		xb_filter_hash_free(&databases_exclude_hash);
	}
}

#ifdef RLIMIT_NOFILE
/**
Set the open files limit. Based on set_max_open_files().
@param max_file_limit  requested open files limit
@return the resulting open files limit. May be less or more than the requested
value.  */
static ulong xb_set_max_open_files(rlim_t max_file_limit)
{
	struct rlimit rlimit;
	rlim_t old_cur;

	if (getrlimit(RLIMIT_NOFILE, &rlimit)) {

		goto end;
	}

	old_cur = rlimit.rlim_cur;

	if (rlimit.rlim_cur == RLIM_INFINITY) {

		rlimit.rlim_cur = max_file_limit;
	}

	if (rlimit.rlim_cur >= max_file_limit) {

		max_file_limit = rlimit.rlim_cur;
		goto end;
	}

	rlimit.rlim_cur = rlimit.rlim_max = max_file_limit;

	if (setrlimit(RLIMIT_NOFILE, &rlimit)) {
		/* Use original value */
		max_file_limit = static_cast<ulong>(old_cur);
	} else {

		rlimit.rlim_cur = 0;	/* Safety if next call fails */

		(void) getrlimit(RLIMIT_NOFILE, &rlimit);

		if (rlimit.rlim_cur) {

			/* If call didn't fail */
			max_file_limit = rlimit.rlim_cur;
		}
	}

end:
	return static_cast<ulong>(max_file_limit);
}
#else
# define xb_set_max_open_files(x) 0UL
#endif

static void stop_backup_threads(bool running)
{
  if (running)
  {
    fputs("mariabackup: Stopping log copying thread", stderr);
    fflush(stderr);
    while (log_copying_running)
    {
      putc('.', stderr);
      fflush(stderr);
      std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    putc('\n', stderr);
    mysql_cond_destroy(&log_copying_stop);
  }

  if (have_io_watching_thread)
  {
    pthread_join(io_watching_thread_id, nullptr);
    mysql_cond_destroy(&wait_throttle);
  }
}

/** Implement the core of --backup
@return	whether the operation succeeded */
static bool xtrabackup_backup_low()
{
	mysql_mutex_lock(&recv_sys.mutex);
	ut_ad(!metadata_to_lsn);

	/* read the latest checkpoint lsn */
	{
		const lsn_t lsn = recv_sys.lsn;
		if (recv_sys.find_checkpoint() == DB_SUCCESS
		    && log_sys.is_latest()) {
			metadata_to_lsn = log_sys.next_checkpoint_lsn;
			msg("mariabackup: The latest check point"
			    " (for incremental): '" LSN_PF "'",
			    metadata_to_lsn);
		} else {
			msg("Error: recv_sys.find_checkpoint() failed.");
		}

		recv_sys.lsn = lsn;
		mysql_cond_broadcast(&log_copying_stop);
		const bool running= log_copying_running;
		mysql_mutex_unlock(&recv_sys.mutex);
		stop_backup_threads(running);
		mysql_mutex_lock(&recv_sys.mutex);
	}

	if (metadata_to_lsn && xtrabackup_copy_logfile()) {
		mysql_mutex_unlock(&recv_sys.mutex);
		ds_close(dst_log_file);
		dst_log_file = NULL;
		return false;
	}

	mysql_mutex_unlock(&recv_sys.mutex);

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
	metadata_last_lsn = recv_sys.lsn;

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
	xb_plugin_backup_init(mysql_connection);
	msg("open files limit requested %lu, set to %lu",
	    xb_open_files_limit,
	    xb_set_max_open_files(xb_open_files_limit));

	mysql_data_home= mysql_data_home_buff;
	mysql_data_home[0]=FN_CURLIB;		// all paths are relative from here
	mysql_data_home[1]=0;

	srv_n_purge_threads = 1;
	srv_read_only_mode = TRUE;

	srv_operation = SRV_OPERATION_BACKUP;
	log_file_op = backup_file_op;
	first_page_init = backup_first_page_op;
	metadata_to_lsn = 0;

	/* initialize components */
        if(innodb_init_param()) {
fail:
		if (log_copying_running) {
			mysql_mutex_lock(&recv_sys.mutex);
			metadata_to_lsn = 1;
			mysql_cond_broadcast(&log_copying_stop);
			mysql_mutex_unlock(&recv_sys.mutex);
			stop_backup_threads(true);
		}

		log_file_op = NULL;
		first_page_init = NULL;
		if (dst_log_file) {
			ds_close(dst_log_file);
			dst_log_file = NULL;
		}
		if (fil_system.is_initialised()) {
			innodb_shutdown();
		}
		return(false);
	}

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
	srv_thread_pool_init();
	/* Reset the system variables in the recovery module. */
	trx_pool_init();
	recv_sys.create();

	xb_filters_init();

	xb_fil_io_init();
	srv_n_file_io_threads = srv_n_read_io_threads;

	if (os_aio_init()) {
		msg("Error: cannot initialize AIO subsystem");
		goto fail;
	}

	log_sys.create();
	/* get current checkpoint_lsn */

	mysql_mutex_lock(&recv_sys.mutex);

	if (recv_sys.find_checkpoint() != DB_SUCCESS) {
		msg("Error: cannot read redo log header");
unlock_and_fail:
		mysql_mutex_unlock(&recv_sys.mutex);
	}

	if (!log_sys.is_latest()) {
		msg("Error: cannot process redo log before MariaDB 10.8");
		goto unlock_and_fail;
	}

	recv_needed_recovery = true;
	mysql_mutex_unlock(&recv_sys.mutex);

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

	xtrabackup_init_datasinks();

	if (!select_history()) {
		goto fail;
	}

	/* open the log file */
	memset(&stat_info, 0, sizeof(MY_STAT));
	dst_log_file = ds_open(ds_redo, LOG_FILE_NAME, &stat_info);
	if (dst_log_file == NULL) {
		msg("Error: failed to open the target stream for '%s'.",
		    LOG_FILE_NAME);
		goto fail;
	}

	/* label it */
	recv_sys.file_checkpoint = log_sys.next_checkpoint_lsn;
	log_hdr_init();
	/* Write log header*/
	if (ds_write(dst_log_file, log_hdr_buf, 12288)) {
		msg("error: write to logfile failed");
		goto fail;
	}
	log_copying_running = true;
	/* start io throttle */
	if(xtrabackup_throttle) {
		io_ticket = xtrabackup_throttle;
		have_io_watching_thread = true;
		mysql_cond_init(0, &wait_throttle, nullptr);
		mysql_thread_create(0, &io_watching_thread_id, nullptr,
				    io_watching_thread, nullptr);
	}

	/* Populate fil_system with tablespaces to copy */
	if (dberr_t err = xb_load_tablespaces()) {
		msg("merror: xb_load_tablespaces() failed with"
		    " error %s.", ut_strerr(err));
		log_copying_running = false;
		goto fail;
	}

	/* copy log file by current position */

	mysql_mutex_lock(&recv_sys.mutex);
	recv_sys.lsn = log_sys.next_checkpoint_lsn;

	const bool log_copy_failed = xtrabackup_copy_logfile();

	mysql_mutex_unlock(&recv_sys.mutex);

	if (log_copy_failed) {
		log_copying_running = false;
		goto fail;
	}

	DBUG_MARIABACKUP_EVENT("before_innodb_log_copy_thread_started", {});

	mysql_cond_init(0, &log_copying_stop, nullptr);
	std::thread(log_copying_thread).detach();

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
			dbug_start_query_thread("ALTER TABLE test.t ADD COLUMN mdl_lock_column int",
				"Waiting for table metadata lock", 0, 0););
	}

	datafiles_iter_t it;

	/* Create data copying threads */
	data_threads = (data_thread_ctxt_t *)
		malloc(sizeof(data_thread_ctxt_t) * xtrabackup_parallel);
	count = xtrabackup_parallel;
	pthread_mutex_init(&count_mutex, NULL);

	for (i = 0; i < (uint) xtrabackup_parallel; i++) {
		data_threads[i].it = &it;
		data_threads[i].num = i+1;
		data_threads[i].count = &count;
		data_threads[i].count_mutex = &count_mutex;
		data_threads[i].corrupted_pages = &corrupted_pages;
		std::thread(data_copy_thread_func, data_threads + i).detach();
	}

	/* Wait for threads to exit */
	while (1) {
		std::this_thread::sleep_for(std::chrono::seconds(1));
		pthread_mutex_lock(&count_mutex);
		bool stop = count == 0;
		pthread_mutex_unlock(&count_mutex);
		if (stop) {
			break;
		}
	}

	pthread_mutex_destroy(&count_mutex);
	free(data_threads);

	bool ok = backup_start(corrupted_pages);

	if (ok) {
		ok = xtrabackup_backup_low();

		backup_release();

		DBUG_EXECUTE_IF("check_mdl_lock_works",
				pthread_join(dbug_alter_thread, nullptr););

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

	msg("Redo log (from LSN " LSN_PF " to " LSN_PF ") was copied.",
	    log_sys.next_checkpoint_lsn, recv_sys.lsn);
	xb_filters_free();

	xb_data_files_close();

	/* Make sure that the latest checkpoint was included */
	if (metadata_to_lsn > recv_sys.lsn) {
		msg("Error: failed to copy enough redo log ("
		    "LSN=" LSN_PF "; checkpoint LSN=" LSN_PF ").",
		    recv_sys.lsn, metadata_to_lsn);
		goto fail;
	}

	innodb_shutdown();
	log_file_op = NULL;
	first_page_init = NULL;
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
	std::set<std::string> dropped_tables;
	std::map<std::string, std::string> renamed_tables;
	space_id_to_name_t new_tables;

	/* Disable further DDL on backed up tables (only needed for --no-lock).*/
	mysql_mutex_lock(&recv_sys.mutex);
	log_file_op = backup_file_op_fail;
	mysql_mutex_unlock(&recv_sys.mutex);

	DBUG_MARIABACKUP_EVENT("backup_fix_ddl", {});

	for (space_id_to_name_t::iterator iter = ddl_tracker.tables_in_backup.begin();
		iter != ddl_tracker.tables_in_backup.end();
		iter++) {

		const std::string name = iter->second;
		uint32_t id = iter->first;

		if (ddl_tracker.drops.find(id) != ddl_tracker.drops.end()) {
			dropped_tables.insert(name);
			corrupted_pages.drop_space(id);
			continue;
		}

		if (ddl_tracker.id_to_name.find(id) == ddl_tracker.id_to_name.end()) {
			continue;
		}

		/* tablespace was affected by DDL. */
		const std::string new_name = ddl_tracker.id_to_name[id];
		if (new_name != name) {
			renamed_tables[name] = new_name;
			if (opt_log_innodb_page_corruption)
				corrupted_pages.rename_space(id, new_name);
		}
	}

	/* Find tables that were created during backup (and not removed).*/
	for(space_id_to_name_t::iterator iter = ddl_tracker.id_to_name.begin();
		iter != ddl_tracker.id_to_name.end();
		iter++) {

		uint32_t id = iter->first;
		std::string name = iter->second;

		if (ddl_tracker.tables_in_backup.find(id) != ddl_tracker.tables_in_backup.end()) {
			/* already processed above */
			continue;
		}

		if (ddl_tracker.drops.find(id) == ddl_tracker.drops.end()
		    && ddl_tracker.deferred_tables.find(id)
			== ddl_tracker.deferred_tables.end()) {
			dropped_tables.erase(name);
			new_tables[id] = name;
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
	datafiles_iter_t it;
	while (fil_node_t *node = datafiles_iter_next(&it)) {
		all_nodes.push_back(node);
	}
	for (size_t i = 0; i < all_nodes.size(); i++) {
		fil_node_t *n = all_nodes[i];
		if (n->space->id == 0)
			continue;
		if (n->is_open()) {
			mysql_mutex_lock(&fil_system.mutex);
			n->close();
			mysql_mutex_unlock(&fil_system.mutex);
		}
		fil_space_free(n->space->id, false);
	}

	DBUG_EXECUTE_IF("check_mdl_lock_works", DBUG_ASSERT(new_tables.size() == 0););

	srv_operation = SRV_OPERATION_BACKUP_NO_DEFER;

	/* Mariabackup detected the FILE_MODIFY or FILE_RENAME
	for the deferred tablespace. So it needs to read the
	tablespace again if innodb doesn't have page0 initialization
	redo log for it */
	for (space_id_to_name_t::iterator iter =
			ddl_tracker.deferred_tables.begin();
	     iter != ddl_tracker.deferred_tables.end();
	     iter++) {
		if (check_if_skip_table(iter->second.c_str())) {
			continue;
		}

		if (first_page_init_ids.find(iter->first)
				!= first_page_init_ids.end()) {
			new_tables[iter->first] = iter->second.c_str();
			continue;
		}

		xb_load_single_table_tablespace(iter->second, false);
	}

	/* Mariabackup doesn't detect any FILE_OP for the deferred
	tablespace. There is a possiblity that page0 could've
	been corrupted persistently in the disk */
	for (auto space_name: defer_space_names) {
		if (!check_if_skip_table(space_name.c_str())) {
			xb_load_single_table_tablespace(
					space_name, false);
		}
	}

	srv_operation = SRV_OPERATION_BACKUP;

	for (const auto &t : new_tables) {
		if (!check_if_skip_table(t.second.c_str())) {
			xb_load_single_table_tablespace(t.second, false,
							t.first);
		}
	}

	datafiles_iter_t it2;

	while (fil_node_t *node = datafiles_iter_next(&it2)) {
		if (!fil_is_user_tablespace_id(node->space->id))
			continue;
		std::string dest_name= filename_to_spacename(
			node->name, strlen(node->name));
		dest_name.append(".new");

		xtrabackup_copy_datafile(node, 0, dest_name.c_str(), wf_write_through,
			corrupted_pages);
	}
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
	uint32_t	space_id,	/*!<in: space id */
	uint32_t	flags,		/*!<in: tablespace flags */
	pfs_os_file_t*	file)		/*!<out: file handle */
{
	bool		ret;

	*file = os_file_create_simple_no_error_handling(
		0, path, OS_FILE_CREATE, OS_FILE_READ_WRITE, false, &ret);
	if (!ret) {
		msg("Can't create file %s", path);
		return ret;
	}

	ret = os_file_set_size(path, *file,
			       FIL_IBD_FILE_INITIAL_SIZE
			       << srv_page_size_shift);
	if (!ret) {
		msg("mariabackup: cannot set size for file %s", path);
		os_file_close(*file);
		os_file_delete(0, path);
		return ret;
	}

	return TRUE;
}

static fil_space_t* fil_space_get_by_name(const char* name)
{
  mysql_mutex_assert_owner(&fil_system.mutex);
  for (fil_space_t &space : fil_system.space_list)
    if (space.chain.start)
      if (const char *str= strstr(space.chain.start->name, name))
        if (!strcmp(str + strlen(name), ".ibd") &&
            (str == space.chain.start->name ||
             IF_WIN(str[-1] == '\\' ||,) str[-1] == '/'))
          return &space;
  return nullptr;
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
	     info.space_id == UINT32_MAX);

	*success = false;

	if (dbname) {
		snprintf(dest_dir, FN_REFLEN, "%s/%s",
			xtrabackup_target_dir, dbname);
		snprintf(dest_space_name, FN_REFLEN, "%s/%s", dbname, name);
	} else {
		snprintf(dest_dir, FN_REFLEN, "%s", xtrabackup_target_dir);
		snprintf(dest_space_name, FN_REFLEN, "%s", name);
	}

	snprintf(real_name, real_name_len,
		 "%s/%s",
		 xtrabackup_target_dir, dest_space_name);
	/* Truncate ".ibd" */
	dest_space_name[strlen(dest_space_name) - 4] = '\0';

	/* Create the database directory if it doesn't exist yet */
	if (!os_file_create_directory(dest_dir, FALSE)) {
		msg("mariabackup: error: cannot create dir %s", dest_dir);
		return file;
	}

	if (!info.space_id && fil_system.sys_space) {
		fil_node_t *node
			= UT_LIST_GET_FIRST(fil_system.sys_space->chain);
		for (; node; node = UT_LIST_GET_NEXT(chain, node)) {
			if (!strcmp(node->name, real_name)) {
				break;
			}
		}
		if (node && node->handle != OS_FILE_CLOSED) {
			*success = true;
			return node->handle;
		}
		msg("mariabackup: Cannot find file %s\n", real_name);
		return OS_FILE_CLOSED;
	}

	mysql_mutex_lock(&recv_sys.mutex);
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
		mysql_mutex_unlock(&recv_sys.mutex);
		return file;
	}

	const size_t len = strlen(dest_space_name);
	/* remember space name for further reference */
	table = static_cast<xb_filter_entry_t *>
		(malloc(sizeof(xb_filter_entry_t) +
			len + 1));

	table->name = ((char*)table) + sizeof(xb_filter_entry_t);
	memcpy(table->name, dest_space_name, len + 1);
	const ulint fold = my_crc32c(0, dest_space_name, len);
	HASH_INSERT(xb_filter_entry_t, name_hash, &inc_dir_tables_hash,
		    fold, table);

	mysql_mutex_lock(&fil_system.mutex);
	fil_space = fil_space_get_by_name(dest_space_name);
	mysql_mutex_unlock(&fil_system.mutex);

	if (fil_space != NULL) {
		if (fil_space->id == info.space_id
		    || info.space_id == UINT32_MAX) {
			/* we found matching space */
			goto found;
		} else {

			char	tmpname[FN_REFLEN];

			snprintf(tmpname, FN_REFLEN, "%s/xtrabackup_tmp_#%u",
				 dbname, fil_space->id);

			msg("mariabackup: Renaming %s to %s.ibd",
				fil_space->chain.start->name, tmpname);

			if (fil_space->rename(tmpname, false) != DB_SUCCESS) {
				msg("mariabackup: Cannot rename %s to %s",
					fil_space->chain.start->name, tmpname);
				goto exit;
			}
		}
	}

	if (info.space_id == UINT32_MAX)
	{
		die("Can't handle DDL operation on tablespace "
		    "%s\n", dest_space_name);
	}
	mysql_mutex_lock(&fil_system.mutex);
	fil_space = fil_space_get_by_id(info.space_id);
	mysql_mutex_unlock(&fil_system.mutex);
	if (fil_space != NULL) {
		char	tmpname[FN_REFLEN];

		snprintf(tmpname, sizeof tmpname, "%s.ibd", dest_space_name);

		msg("mariabackup: Renaming %s to %s",
		    fil_space->chain.start->name, tmpname);

		if (fil_space->rename(tmpname, false) != DB_SUCCESS) {
			msg("mariabackup: Cannot rename %s to %s",
			    fil_space->chain.start->name, tmpname);
			goto exit;
		}

		goto found;
	}

	/* No matching space found. create the new one.  */
	const uint32_t flags = info.zip_size
		? get_bit_shift(info.page_size
				>> (UNIV_ZIP_SIZE_SHIFT_MIN - 1))
		<< FSP_FLAGS_POS_ZIP_SSIZE
		| FSP_FLAGS_MASK_POST_ANTELOPE
		| FSP_FLAGS_MASK_ATOMIC_BLOBS
		| (srv_page_size == UNIV_PAGE_SIZE_ORIG
		   ? 0
		   : get_bit_shift(srv_page_size
				   >> (UNIV_ZIP_SIZE_SHIFT_MIN - 1))
		   << FSP_FLAGS_POS_PAGE_SSIZE)
		: FSP_FLAGS_PAGE_SSIZE();
	ut_ad(fil_space_t::zip_size(flags) == info.zip_size);
	ut_ad(fil_space_t::physical_size(flags) == info.page_size);

	if (fil_space_t::create(info.space_id, flags,
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

	xb_delta_info_t info(srv_page_size, 0, SRV_TMP_SPACE_ID);
	ulint		page_size;
	ulint		page_size_shift;
	byte*		incremental_buffer = NULL;

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

	if (!xb_read_delta_metadata(meta_path, &info)) {
		goto error;
	}

	page_size = info.page_size;
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
	incremental_buffer = static_cast<byte *>
		(aligned_malloc(page_size / 4 * page_size, page_size));

	msg("Applying %s to %s...", src_path, dst_path);

	while (!last_buffer) {
		ulint cluster_header;

		/* read to buffer */
		/* first block of block cluster */
		offset = ((incremental_buffers * (page_size / 4))
			 << page_size_shift);
		if (os_file_read(IORequestRead, src_file,
				 incremental_buffer, offset, page_size)
		    != DB_SUCCESS) {
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
		if (os_file_read(IORequestRead, src_file,
				 incremental_buffer,
				 offset, page_in_buffer * page_size)
		    != DB_SUCCESS) {
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
					   = fil_system.sys_space) {
					/* The system tablespace can
					consist of multiple files. The
					first one has full tablespace
					size in page 0, but only the last
					file should be extended. */
					fil_node_t* n = UT_LIST_GET_FIRST(
						space->chain);
					bool fail = !strcmp(n->name, dst_path)
						&& !fil_space_extend(
							space, uint32_t(n_pages));
					if (fail) goto error;
				}
			}

			if (os_file_write(IORequestWrite,
					  dst_path, dst_file, buf, off,
					  page_size) != DB_SUCCESS) {
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

	aligned_free(incremental_buffer);
	if (src_file != OS_FILE_CLOSED) {
		os_file_close(src_file);
		os_file_delete(0,src_path);
	}
	if (dst_file != OS_FILE_CLOSED && info.space_id)
		os_file_close(dst_file);
	return TRUE;

error:
	aligned_free(incremental_buffer);
	if (src_file != OS_FILE_CLOSED)
		os_file_close(src_file);
	if (dst_file != OS_FILE_CLOSED && info.space_id)
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
		const ulint fold = my_crc32c(0, table->name,
					     table_name.size());
		HASH_INSERT(xb_filter_entry_t, name_hash, &inc_dir_tables_hash,
			    fold, table);
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
	const size_t len = strlen(name) - 4;
	name[len] = '\0';
	const ulint fold = my_crc32c(0, name, len);

	HASH_SEARCH(name_hash, &inc_dir_tables_hash, fold,
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
	char		dbpath[OS_FILE_MAX_PATH+2];
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
	dbdir = os_file_opendir(path);
	if (UNIV_UNLIKELY(dbdir != IF_WIN(INVALID_HANDLE_VALUE, nullptr))) {
		ret = fil_file_readdir_next_file(&err, path, dbdir, &fileinfo);
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
	dir = os_file_opendir(path);

	if (UNIV_UNLIKELY(dbdir == IF_WIN(INVALID_HANDLE_VALUE, nullptr))) {
		msg("Can't open dir %s", path);
		return TRUE;
	}

	ret = fil_file_readdir_next_file(&err, path, dir, &dbinfo);
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

		dbdir = os_file_opendir(dbpath);

		if (dbdir != IF_WIN(INVALID_HANDLE_VALUE, nullptr)) {
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
			die("ERROR : File %s was not found, but expected during rename processing\n", ren2.c_str());
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
	if (xtrabackup_incremental)
		inc_dir_tables_hash.create(1000);

	msg("open files limit requested %u, set to %lu",
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
	xb_plugin_prepare_init(argc, argv, xtrabackup_incremental_dir);

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
	srv_n_purge_threads = 1;

	xb_filters_init();

	srv_log_group_home_dir = NULL;

	if (xtrabackup_incremental) {
		srv_operation = SRV_OPERATION_RESTORE_DELTA;

		if (innodb_init_param()) {
error:
			ok = false;
			goto cleanup;
		}

		recv_sys.create();
		log_sys.create();
		recv_sys.recovery_on = true;

		xb_fil_io_init();
		if (dberr_t err = xb_load_tablespaces()) {
			msg("mariabackup: error: xb_data_files_init() failed "
			    "with error %s\n", ut_strerr(err));
			goto error;
		}

		ok = fil_system.sys_space->open(false)
			&& xtrabackup_apply_deltas();

		xb_data_files_close();

		if (ok) {
			/* Cleanup datadir from tablespaces deleted
			between full and incremental backups */

			xb_process_datadir("./", ".ibd", rm_if_not_found);
		}

		xb_filter_hash_free(&inc_dir_tables_hash);

		fil_system.close();
		innodb_free_param();
		log_sys.close();
		if (!ok) goto cleanup;
	}

	srv_operation = xtrabackup_export
		? SRV_OPERATION_RESTORE_EXPORT : SRV_OPERATION_RESTORE;

	if (innodb_init_param()) {
		goto error;
	}

	fil_system.freeze_space_list = 0;

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

        if (innodb_init()) {
		goto error;
	}

	ut_ad(!fil_system.freeze_space_list);

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

	if (ok) {
		msg("Last binlog file %s, position %lld",
		    trx_sys.recovered_binlog_filename,
		    longlong(trx_sys.recovered_binlog_offset));
	}

	/* Check whether the log is applied enough or not. */
	if (recv_sys.lsn && recv_sys.lsn < target_lsn) {
		msg("mariabackup: error: "
		    "The log was only applied up to LSN " LSN_PF
		    ", instead of " LSN_PF, recv_sys.lsn, target_lsn);
		ok = false;
	}
#ifdef WITH_WSREP
	else if (ok) xb_write_galera_info(xtrabackup_incremental);
#endif

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

cleanup:
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


/**
Check DB user privileges according to the intended actions.

Fetches DB user privileges, determines intended actions based on
command-line arguments and prints missing privileges.
@return whether all the necessary privileges are granted */
static bool check_all_privileges()
{
	if (!mysql_connection) {
		/* Not connected, no queries is going to be executed. */
		return true;
	}

	/* Fetch effective privileges. */
	std::list<std::string> granted_privileges;
	MYSQL_RES* result = xb_mysql_query(mysql_connection, "SHOW GRANTS",
					   true);
	while (MYSQL_ROW row = mysql_fetch_row(result)) {
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
		msg("Current privileges, as reported by 'SHOW GRANTS': ");
		int n=1;
		for (std::list<std::string>::const_iterator it = granted_privileges.begin();
			it != granted_privileges.end();
			it++,n++) {
				msg("  %d.%s", n, it->c_str());
		}
		return false;
	}

	return true;
}

static
void
xb_init_buffer_pool(const char * filename)
{
	if (filename &&
#ifdef _WIN32
		(filename[0] == '/'  ||
		 filename[0] == '\\' ||
		 strchr(filename, ':')))
#else
		filename[0] == FN_LIBCHAR)
#endif
	{
		buffer_pool_filename = strdup(filename);
	} else {
		char filepath[FN_REFLEN];
		char *dst_dir =
			(innobase_data_home_dir && *innobase_data_home_dir) ?
			 innobase_data_home_dir : mysql_data_home;
		size_t dir_length;
		if (dst_dir && *dst_dir) {
			dir_length = strlen(dst_dir);
			while (IS_TRAILING_SLASH(dst_dir, dir_length)) {
				dir_length--;
			}
			memcpy(filepath, dst_dir, dir_length);
		}
		else {
			filepath[0] = '.';
			dir_length = 1;
		}
		snprintf(filepath + dir_length,
			sizeof(filepath) - dir_length, "%c%s", FN_LIBCHAR,
			filename ? filename : "ib_buffer_pool");
		buffer_pool_filename = strdup(filepath);
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

		xb_init_buffer_pool(buffer_pool_filename);

		if (opt_check_privileges && !check_all_privileges()) {
			return(false);
		}

		history_start_time = time(NULL);
	} else {
		xb_init_buffer_pool(innobase_buffer_pool_filename);
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

/** Handle mariabackup options. The options are handled with the following
order:

1) Load server groups and process server options, ignore unknown options
2) Load client groups and process client options, ignore unknown options
3) Load backup groups and process client-server options, exit on unknown option
4) Process --mysqld-args options, ignore unknown options

@param[in] argc arguments count
@param[in] argv arguments array
@param[out] argv_server server options including loaded from server groups
@param[out] argv_client client options including loaded from client groups
@param[out] argv_backup backup options including loaded from backup groups */
void handle_options(int argc, char **argv, char ***argv_server,
                    char ***argv_client, char ***argv_backup)
{
	/* Setup some variables for Innodb.*/
	srv_operation = SRV_OPERATION_RESTORE;

	files_charset_info = &my_charset_utf8mb3_general_ci;


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

        // array_elements() will not work for load_defaults, as it is defined
        // as external symbol, so let's use dynamic array to have ability to
        // add new server default groups
        std::vector<const char *> server_default_groups;

        for (const char **default_group= load_default_groups; *default_group;
             ++default_group)
          server_default_groups.push_back(*default_group);

        std::vector<char *> mysqld_args;
        std::vector<char *> mariabackup_args;
        mysqld_args.push_back(argv[0]);
        mariabackup_args.push_back(argv[0]);

        /* scan options for group and config file to load defaults from */
        for (i= 1; i < argc; i++)
        {
          char *optend= strcend(argv[i], '=');
          if (mysqld_args.size() > 1 ||
              strncmp(argv[i], "--mysqld-args", optend - argv[i]) == 0)
          {
            mysqld_args.push_back(argv[i]);
            continue;
          }
          else
            mariabackup_args.push_back(argv[i]);

          if (strncmp(argv[i], "--defaults-group", optend - argv[i]) == 0)
          {
            defaults_group= optend + 1;
            server_default_groups.push_back(defaults_group);
          }
          else if (strncmp(argv[i], "--login-path", optend - argv[i]) == 0)
          {
            append_defaults_group(optend + 1, xb_client_default_groups,
                                  array_elements(xb_client_default_groups));
          }
          else if (!strncmp(argv[i], "--prepare", optend - argv[i]))
          {
            prepare= true;
          }
          else if (!strncmp(argv[i], "--apply-log", optend - argv[i]))
          {
            prepare= true;
          }
          else if (!strncmp(argv[i], "--incremental-dir", optend - argv[i]) &&
                   *optend)
          {
            target_dir= optend + 1;
          }
          else if (!strncmp(argv[i], "--target-dir", optend - argv[i]) &&
                   *optend && !target_dir)
          {
            target_dir= optend + 1;
          }
          else if (!*optend && argv[i][0] != '-' && !target_dir)
          {
            target_dir= argv[i];
          }
        }

        server_default_groups.push_back(NULL);
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

        mariabackup_args.push_back(nullptr);
        *argv_client= *argv_server= *argv_backup= &mariabackup_args[0];
        int argc_backup= static_cast<int>(mariabackup_args.size() - 1);
        int argc_client= argc_backup;
        int argc_server= argc_backup;

        /* 1) Load server groups and process server options, ignore unknown
         options */

        load_defaults_or_exit(conf_file, &server_default_groups[0],
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

        /* 2) Load client groups and process client options, ignore unknown
         options */

	load_defaults_or_exit(conf_file, xb_client_default_groups,
			      &argc_client, argv_client);

	for (n = 0; (*argv_client)[n]; n++) {};
 	argc_client = n;

	if (innobackupex_mode && argc_client > 0) {
		if (!ibx_handle_options(&argc_client, argv_client)) {
			exit(EXIT_FAILURE);
		}
	}

	if (argc_client > 0
	    && (ho_error=handle_options(&argc_client, argv_client,
					xb_client_options, xb_get_one_option)))
		exit(ho_error);

        /* 3) Load backup groups and process client-server options, exit on
         unknown option */

        load_defaults_or_exit(conf_file, backup_default_groups, &argc_backup,
                              argv_backup);
        for (n= 0; (*argv_backup)[n]; n++)
        {
        };
        argc_backup= n;

        my_handle_options_init_variables = FALSE;

        if (argc_backup > 0 &&
            (ho_error= handle_options(&argc_backup, argv_backup,
                                      xb_server_options, xb_get_one_option)))
          exit(ho_error);

        /* Add back the program name handle_options removes */
        ++argc_backup;
        --(*argv_backup);

        if (innobackupex_mode && argc_backup > 0 &&
            !ibx_handle_options(&argc_backup, argv_backup))
          exit(EXIT_FAILURE);

        my_getopt_skip_unknown = FALSE;

        if (argc_backup > 0 &&
            (ho_error= handle_options(&argc_backup, argv_backup,
                                      xb_client_options, xb_get_one_option)))
          exit(ho_error);

        if (opt_password)
        {
          char *argument= (char*) opt_password;
          char *start= (char*) opt_password;
          opt_password= my_strdup(PSI_NOT_INSTRUMENTED, opt_password,
                                  MYF(MY_FAE));
          while (*argument)
            *argument++= 'x'; // Destroy argument
          if (*start)
            start[1]= 0;
        }

        /* 4) Process --mysqld-args options, ignore unknown options */

        my_getopt_skip_unknown = TRUE;

        int argc_mysqld = static_cast<int>(mysqld_args.size());
        if (argc_mysqld > 1)
        {
          char **argv_mysqld= &mysqld_args[0];
          if ((ho_error= handle_options(&argc_mysqld, &argv_mysqld,
                                        xb_server_options, xb_get_one_option)))
            exit(ho_error);
        }

        my_handle_options_init_variables = TRUE;

	/* Reject command line arguments that don't look like options, i.e. are
	not of the form '-X' (single-character options) or '--option' (long
	options) */
	for (int i = 0 ; i < argc_backup ; i++) {
		const char * const opt = (*argv_backup)[i];

		if (strncmp(opt, "--", 2) &&
		    !(strlen(opt) == 2 && opt[0] == '-')) {
			bool server_option = true;

			for (int j = 0; j < argc_backup; j++) {
				if (opt == (*argv_backup)[j]) {
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
  char **server_defaults;
  char **client_defaults;
  char **backup_defaults;

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

	xb_regex_init();

	capture_tool_command(argc, argv);

	if (mysql_server_init(-1, NULL, NULL))
	{
		die("mysql_server_init() failed");
	}

	system_charset_info = &my_charset_utf8mb3_general_ci;
	key_map_full.set_all();

	logger.init_base();
	logger.set_handlers(LOG_NONE, LOG_NONE);
	mysql_mutex_init(key_LOCK_error_log, &LOCK_error_log,
			 MY_MUTEX_INIT_FAST);

        handle_options(argc, argv, &server_defaults, &client_defaults,
                       &backup_defaults);

#ifndef DBUG_OFF
	if (dbug_option) {
		DBUG_SET_INITIAL(dbug_option);
		DBUG_SET(dbug_option);
	}
#endif
	/* Main functions for library */
	init_thr_timer(5);

	int status = main_low(server_defaults);

	end_thr_timer();
	backup_cleanup();

	if (innobackupex_mode) {
		ibx_cleanup();
	}

	free_defaults(server_defaults);
        free_defaults(client_defaults);
        free_defaults(backup_defaults);

#ifndef DBUG_OFF
	if (dbug_option) {
		DBUG_END();
	}
#endif

	logger.cleanup_base();
	cleanup_errmsgs();
	free_error_messages();
	mysql_mutex_destroy(&LOCK_error_log);

	free(buffer_pool_filename);

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
#if defined(_WIN32)
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

	if (xtrabackup_export && !srv_file_per_table) {
		msg("mariabackup: auto-enabling --innodb-file-per-table due to "
		    "the --export option");
		srv_file_per_table = TRUE;
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

	ut_ad(!field_ref_zero);
	if (auto b = aligned_malloc(UNIV_PAGE_SIZE_MAX, 4096)) {
		field_ref_zero = static_cast<byte*>(
			memset_aligned<4096>(b, 0, UNIV_PAGE_SIZE_MAX));
	} else {
		msg("Can't allocate memory for field_ref_zero");
		return EXIT_FAILURE;
	}

	auto _ = make_scope_exit([]() {
		aligned_free(const_cast<byte*>(field_ref_zero));
		field_ref_zero = nullptr;
		});

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
  DWORD ret = GetModuleFileNameA(NULL, buf, (DWORD)size);
  if (ret > 0)
    return 0;
#elif defined(__linux__)
  ssize_t ret = readlink("/proc/self/exe", buf, size-1);
  if(ret > 0)
    return 0;
#elif defined(__APPLE__)
  size_t ret = proc_pidpath(getpid(), buf, static_cast<uint32_t>(size));
  if (ret > 0) {
    buf[ret] = 0;
    return 0;
  }
#elif defined(__FreeBSD__)
  int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_PATHNAME, -1};
  if (sysctl(mib, 4, buf, &size, NULL, 0) == 0) {
    return 0;
  }
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
