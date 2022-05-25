/* Copyright (c) 2000, 2015, Oracle and/or its affiliates.
   Copyright (c) 2008, 2022, MariaDB

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1335  USA */

#include "sql_plugin.h"                         // Includes mariadb.h
#include "sql_priv.h"
#include "unireg.h"
#include <signal.h>
#ifndef _WIN32
#include <netdb.h>        // getservbyname, servent
#endif
#include "sql_parse.h"    // path_starts_from_data_home_dir
#include "sql_cache.h"    // query_cache, query_cache_*
#include "sql_locale.h"   // MY_LOCALES, my_locales, my_locale_by_name
#include "sql_show.h"     // free_status_vars, add_status_vars,
                          // reset_status_vars
#include "strfunc.h"      // find_set_from_flags
#include "parse_file.h"   // File_parser_dummy_hook
#include "sql_db.h"       // my_dboptions_cache_free
                          // my_dboptions_cache_init
#include "sql_table.h"    // ddl_log_release, ddl_log_execute_recovery
#include "sql_connect.h"  // free_max_user_conn, init_max_user_conn,
                          // handle_one_connection
#include "thread_cache.h"
#include "sql_time.h"     // known_date_time_formats,
                          // get_date_time_format_str,
                          // date_time_format_make
#include "tztime.h"       // my_tz_free, my_tz_init, my_tz_SYSTEM
#include "hostname.h"     // hostname_cache_free, hostname_cache_init
#include "sql_acl.h"      // acl_free, grant_free, acl_init,
                          // grant_init
#include "sql_base.h"
#include "sql_test.h"     // mysql_print_status
#include "item_create.h"  // item_create_cleanup, item_create_init
#include "sql_servers.h"  // servers_free, servers_init
#include "init.h"         // unireg_init
#include "derror.h"       // init_errmessage
#include "des_key_file.h" // load_des_key_file
#include "sql_manager.h"  // stop_handle_manager, start_handle_manager
#include "sql_expression_cache.h" // subquery_cache_miss, subquery_cache_hit
#include "sys_vars_shared.h"
#include "ddl_log.h"

#include <m_ctype.h>
#include <my_dir.h>
#include <my_bit.h>
#include "my_cpu.h"
#include "slave.h"
#include "rpl_mi.h"
#include "sql_repl.h"
#include "rpl_filter.h"
#include "client_settings.h"
#include "repl_failsafe.h"
#include <sql_common.h>
#include <my_stacktrace.h>
#include "mysqld_suffix.h"
#include "mysys_err.h"
#include "events.h"
#include "sql_audit.h"
#include "probes_mysql.h"
#include "scheduler.h"
#include <waiting_threads.h>
#include "debug_sync.h"
#include "wsrep_mysqld.h"
#include "wsrep_var.h"
#ifdef WITH_WSREP
#include "wsrep_thd.h"
#include "wsrep_sst.h"
#endif /* WITH_WSREP */
#include "proxy_protocol.h"

#include "sql_callback.h"
#include "threadpool.h"

#ifdef HAVE_OPENSSL
#include <ssl_compat.h>
#endif

#ifdef WITH_PERFSCHEMA_STORAGE_ENGINE
#include "../storage/perfschema/pfs_server.h"
#endif /* WITH_PERFSCHEMA_STORAGE_ENGINE */
#include <mysql/psi/mysql_idle.h>
#include <mysql/psi/mysql_socket.h>
#include <mysql/psi/mysql_statement.h>
#include "mysql_com_server.h"

#include "keycaches.h"
#include "../storage/myisam/ha_myisam.h"
#include "set_var.h"

#include "rpl_injector.h"
#include "semisync_master.h"
#include "semisync_slave.h"

#include "transaction.h"

#ifdef HAVE_SYS_PRCTL_H
#include <sys/prctl.h>
#endif

#include <thr_alarm.h>
#include <ft_global.h>
#include <errmsg.h>
#include "sp_rcontext.h"
#include "sp_cache.h"
#include "sql_reload.h"  // reload_acl_and_cache
#include "sp_head.h"  // init_sp_psi_keys

#include <mysqld_default_groups.h>

#ifdef HAVE_POLL_H
#include <poll.h>
#endif

#ifdef _WIN32
#include <handle_connections_win.h>
#include <sddl.h>
#include <winservice.h> /* SERVICE_STOPPED, SERVICE_RUNNING etc */
#endif

#include <my_service_manager.h>

#define mysqld_charset &my_charset_latin1


extern "C" {					// Because of SCO 3.2V4.2
#include <sys/stat.h>
#ifndef __GNU_LIBRARY__
#define __GNU_LIBRARY__				// Skip warnings in getopt.h
#endif
#include <my_getopt.h>
#ifdef HAVE_SYSENT_H
#include <sysent.h>
#endif
#ifdef HAVE_PWD_H
#include <pwd.h>				// For struct passwd
#endif
#include <my_net.h>

#if !defined(_WIN32)
#include <sys/resource.h>
#ifdef HAVE_SYS_UN_H
#include <sys/un.h>
#endif
#ifdef HAVE_SELECT_H
#include <select.h>
#endif
#ifdef HAVE_SYS_SELECT_H
#include <sys/select.h>
#endif
#include <sys/utsname.h>
#endif /* _WIN32 */

#include <my_libwrap.h>

#ifdef _WIN32 
#include <crtdbg.h>
#endif

#ifdef _AIX41
int initgroups(const char *,unsigned int);
#endif

#if defined(__FreeBSD__) && defined(HAVE_IEEEFP_H) && !defined(HAVE_FEDISABLEEXCEPT)
#include <ieeefp.h>
#ifdef HAVE_FP_EXCEPT				// Fix type conflict
typedef fp_except fp_except_t;
#endif
#endif /* __FreeBSD__ && HAVE_IEEEFP_H && !HAVE_FEDISABLEEXCEPT */
#ifdef HAVE_SYS_FPU_H
/* for IRIX to use set_fpc_csr() */
#include <sys/fpu.h>
#endif
#ifdef HAVE_FPU_CONTROL_H
#include <fpu_control.h>
#endif
#if defined(__i386__) && !defined(HAVE_FPU_CONTROL_H)
# define fpu_control_t unsigned int
# define _FPU_EXTENDED 0x300
# define _FPU_DOUBLE 0x200
# if defined(__GNUC__) || (defined(__SUNPRO_CC) && __SUNPRO_CC >= 0x590)
#  define _FPU_GETCW(cw) asm volatile ("fnstcw %0" : "=m" (*&cw))
#  define _FPU_SETCW(cw) asm volatile ("fldcw %0" : : "m" (*&cw))
# else
#  define _FPU_GETCW(cw) (cw= 0)
#  define _FPU_SETCW(cw)
# endif
#endif

#ifndef HAVE_FCNTL
#define fcntl(X,Y,Z) 0
#endif

inline void setup_fpu()
{
#if defined(__FreeBSD__) && defined(HAVE_IEEEFP_H) && !defined(HAVE_FEDISABLEEXCEPT)
  /* We can't handle floating point exceptions with threads, so disable
     this on freebsd
     Don't fall for overflow, underflow,divide-by-zero or loss of precision.
     fpsetmask() is deprecated in favor of fedisableexcept() in C99.
  */
#if defined(FP_X_DNML)
  fpsetmask(~(FP_X_INV | FP_X_DNML | FP_X_OFL | FP_X_UFL | FP_X_DZ |
	      FP_X_IMP));
#else
  fpsetmask(~(FP_X_INV |             FP_X_OFL | FP_X_UFL | FP_X_DZ |
              FP_X_IMP));
#endif /* FP_X_DNML */
#endif /* __FreeBSD__ && HAVE_IEEEFP_H && !HAVE_FEDISABLEEXCEPT */

#ifdef HAVE_FEDISABLEEXCEPT
  fedisableexcept(FE_ALL_EXCEPT);
#endif

#ifdef HAVE_FESETROUND
    /* Set FPU rounding mode to "round-to-nearest" */
  fesetround(FE_TONEAREST);
#endif /* HAVE_FESETROUND */

  /*
    x86 (32-bit) requires FPU precision to be explicitly set to 64 bit
    (double precision) for portable results of floating point operations.
    However, there is no need to do so if compiler is using SSE2 for floating
    point, double values will be stored and processed in 64 bits anyway.
  */
#if defined(__i386__) && !defined(__SSE2_MATH__)
#if defined(_WIN32)
#if !defined(_WIN64)
  _control87(_PC_53, MCW_PC);
#endif /* !_WIN64 */
#else /* !_WIN32 */
  fpu_control_t cw;
  _FPU_GETCW(cw);
  cw= (cw & ~_FPU_EXTENDED) | _FPU_DOUBLE;
  _FPU_SETCW(cw);
#endif /* _WIN32 && */
#endif /* __i386__ */

#if defined(__sgi) && defined(HAVE_SYS_FPU_H)
  /* Enable denormalized DOUBLE values support for IRIX */
  union fpc_csr n;
  n.fc_word = get_fpc_csr();
  n.fc_struct.flush = 0;
  set_fpc_csr(n.fc_word);
#endif
}

} /* cplusplus */

#define MYSQL_KILL_SIGNAL SIGTERM

#include <my_pthread.h>			// For thr_setconcurency()

#ifdef SOLARIS
extern "C" int gethostname(char *name, int namelen);
#endif

extern "C" sig_handler handle_fatal_signal(int sig);

#if defined(__linux__)
#define ENABLE_TEMP_POOL 1
#else
#define ENABLE_TEMP_POOL 0
#endif

int init_io_cache_encryption();

/* Constants */

#include <welcome_copyright_notice.h> // ORACLE_WELCOME_COPYRIGHT_NOTICE

const char *show_comp_option_name[]= {"YES", "NO", "DISABLED"};

static const char *tc_heuristic_recover_names[]=
{
  "OFF", "COMMIT", "ROLLBACK", NullS
};
static TYPELIB tc_heuristic_recover_typelib=
{
  array_elements(tc_heuristic_recover_names)-1,"",
  tc_heuristic_recover_names, NULL
};

const char *first_keyword= "first";
const char *my_localhost= "localhost", *delayed_user= "DELAYED";

bool opt_large_files= sizeof(my_off_t) > 4;
static my_bool opt_autocommit; ///< for --autocommit command-line option
/*
  Used with --help for detailed option
*/
static my_bool opt_verbose= 0;

/* Timer info to be used by the SQL layer */
MY_TIMER_INFO sys_timer_info;

/* static variables */

#ifdef HAVE_PSI_INTERFACE
#ifdef HAVE_OPENSSL10
static PSI_rwlock_key key_rwlock_openssl;
#endif
#endif /* HAVE_PSI_INTERFACE */

/**
  Statement instrumentation key for replication.
*/
#ifdef HAVE_PSI_STATEMENT_INTERFACE
PSI_statement_info stmt_info_rpl;
#endif

/* the default log output is log tables */
static bool lower_case_table_names_used= 0;
static bool volatile select_thread_in_use, signal_thread_in_use;
static my_bool opt_debugging= 0, opt_external_locking= 0, opt_console= 0;
static my_bool opt_short_log_format= 0, opt_silent_startup= 0;

ulong max_used_connections;
static const char *mysqld_user, *mysqld_chroot;
static char *default_character_set_name;
static char *character_set_filesystem_name;
static char *lc_messages;
static char *lc_time_names_name;
char *my_bind_addr_str;
static char *default_collation_name;
char *default_storage_engine, *default_tmp_storage_engine;
char *enforced_storage_engine=NULL;
char *gtid_pos_auto_engines;
plugin_ref *opt_gtid_pos_auto_plugins;
static char compiled_default_collation_name[]= MYSQL_DEFAULT_COLLATION_NAME;
Thread_cache thread_cache;
static bool binlog_format_used= false;
LEX_STRING opt_init_connect, opt_init_slave;
static DYNAMIC_ARRAY all_options;
static longlong start_memory_used;

/* Global variables */

bool opt_bin_log, opt_bin_log_used=0, opt_ignore_builtin_innodb= 0;
bool opt_bin_log_compress;
uint opt_bin_log_compress_min_len;
my_bool opt_log, debug_assert_if_crashed_table= 0, opt_help= 0;
my_bool debug_assert_on_not_freed_memory= 0;
my_bool disable_log_notes, opt_support_flashback= 0;
static my_bool opt_abort;
ulonglong log_output_options;
my_bool opt_userstat_running;
bool opt_error_log= IF_WIN(1,0);
bool opt_disable_networking=0, opt_skip_show_db=0;
bool opt_skip_name_resolve=0;
my_bool opt_character_set_client_handshake= 1;
bool opt_endinfo, using_udf_functions;
my_bool locked_in_memory;
bool opt_using_transactions;
bool volatile abort_loop;
uint volatile global_disable_checkpoint;
#if defined(_WIN32)
ulong slow_start_timeout;
#endif
static MEM_ROOT startup_root;
MEM_ROOT read_only_root;

/**
   @brief 'grant_option' is used to indicate if privileges needs
   to be checked, in which case the lock, LOCK_grant, is used
   to protect access to the grant table.
   @note This flag is dropped in 5.1
   @see grant_init()
 */
bool volatile grant_option;

my_bool opt_skip_slave_start = 0; ///< If set, slave is not autostarted
my_bool opt_reckless_slave = 0;
my_bool opt_enable_named_pipe= 0;
my_bool opt_local_infile, opt_slave_compressed_protocol;
my_bool opt_safe_user_create = 0;
my_bool opt_show_slave_auth_info;
my_bool opt_log_slave_updates= 0;
my_bool opt_replicate_annotate_row_events= 0;
my_bool opt_mysql56_temporal_format=0, strict_password_validation= 1;
my_bool opt_explicit_defaults_for_timestamp= 0;
char *opt_slave_skip_errors;
char *opt_slave_transaction_retry_errors;

/*
  Legacy global handlerton. These will be removed (please do not add more).
*/
handlerton *heap_hton;
handlerton *myisam_hton;
handlerton *partition_hton;

my_bool read_only= 0, opt_readonly= 0;
my_bool use_temp_pool, relay_log_purge;
my_bool relay_log_recovery;
my_bool opt_sync_frm, opt_allow_suspicious_udfs;
my_bool opt_secure_auth= 0;
my_bool opt_require_secure_transport= 0;
char* opt_secure_file_priv;
my_bool lower_case_file_system= 0;
my_bool opt_large_pages= 0;
my_bool opt_super_large_pages= 0;
my_bool opt_myisam_use_mmap= 0;
uint   opt_large_page_size= 0;
#if defined(ENABLED_DEBUG_SYNC)
MYSQL_PLUGIN_IMPORT uint    opt_debug_sync_timeout= 0;
#endif /* defined(ENABLED_DEBUG_SYNC) */
my_bool opt_old_style_user_limits= 0, trust_function_creators= 0;
ulong opt_replicate_events_marked_for_skip;

/*
  True if there is at least one per-hour limit for some user, so we should
  check them before each query (and possibly reset counters when hour is
  changed). False otherwise.
*/
volatile bool mqh_used = 0;
my_bool opt_noacl;
my_bool sp_automatic_privileges= 1;

ulong opt_binlog_rows_event_max_size;
ulong binlog_row_metadata;
my_bool opt_master_verify_checksum= 0;
my_bool opt_slave_sql_verify_checksum= 1;
const char *binlog_format_names[]= {"MIXED", "STATEMENT", "ROW", NullS};
volatile sig_atomic_t calling_initgroups= 0; /**< Used in SIGSEGV handler. */
uint mysqld_port, select_errors, dropping_tables, ha_open_options;
uint mysqld_extra_port;
uint mysqld_port_timeout;
ulong delay_key_write_options;
uint protocol_version;
uint lower_case_table_names;
ulong tc_heuristic_recover= 0;
Atomic_counter<uint32_t> THD_count::count, CONNECT::count;
bool shutdown_wait_for_slaves;
Atomic_counter<uint32_t> slave_open_temp_tables;
ulong thread_created;
ulong back_log, connect_timeout, server_id;
ulong what_to_log;
ulong slow_launch_time;
ulong open_files_limit, max_binlog_size;
ulong slave_trans_retries;
ulong slave_trans_retry_interval;
uint  slave_net_timeout;
ulong slave_exec_mode_options;
ulong slave_run_triggers_for_rbr= 0;
ulong slave_ddl_exec_mode_options= SLAVE_EXEC_MODE_IDEMPOTENT;
ulonglong slave_type_conversions_options;
ulong thread_cache_size=0;
ulonglong binlog_cache_size=0;
ulonglong binlog_file_cache_size=0;
ulonglong max_binlog_cache_size=0;
ulong slave_max_allowed_packet= 0;
ulonglong binlog_stmt_cache_size=0;
ulonglong  max_binlog_stmt_cache_size=0;
ulonglong test_flags;
ulonglong query_cache_size=0;
ulong query_cache_limit=0;
ulong executed_events=0;
Atomic_counter<query_id_t> global_query_id;
ulong aborted_threads, aborted_connects, aborted_connects_preauth;
ulong delayed_insert_timeout, delayed_insert_limit, delayed_queue_size;
ulong delayed_insert_threads, delayed_insert_writes, delayed_rows_in_use;
ulong delayed_insert_errors,flush_time;
ulong malloc_calls;
ulong specialflag=0;
ulong binlog_cache_use= 0, binlog_cache_disk_use= 0;
ulong binlog_stmt_cache_use= 0, binlog_stmt_cache_disk_use= 0;
ulong max_connections, max_connect_errors;
uint max_password_errors;
ulong extra_max_connections;
uint max_digest_length= 0;
ulong slave_retried_transactions;
ulong transactions_multi_engine;
ulong rpl_transactions_multi_engine;
ulong transactions_gtid_foreign_engine;
ulonglong slave_skipped_errors;
ulong feature_files_opened_with_delayed_keys= 0, feature_check_constraint= 0;
ulonglong denied_connections;
my_decimal decimal_zero;
long opt_secure_timestamp;
uint default_password_lifetime;
my_bool disconnect_on_expired_password;

bool max_user_connections_checking=0;
/**
  Limit of the total number of prepared statements in the server.
  Is necessary to protect the server against out-of-memory attacks.
*/
uint max_prepared_stmt_count;
/**
  Current total number of prepared statements in the server. This number
  is exact, and therefore may not be equal to the difference between
  `com_stmt_prepare' and `com_stmt_close' (global status variables), as
  the latter ones account for all registered attempts to prepare
  a statement (including unsuccessful ones).  Prepared statements are
  currently connection-local: if the same SQL query text is prepared in
  two different connections, this counts as two distinct prepared
  statements.
*/
uint prepared_stmt_count=0;
my_thread_id global_thread_id= 0;
ulong current_pid;
ulong slow_launch_threads = 0;
uint sync_binlog_period= 0, sync_relaylog_period= 0,
     sync_relayloginfo_period= 0, sync_masterinfo_period= 0;
double expire_logs_days = 0;
ulong binlog_expire_logs_seconds = 0;

/**
  Soft upper limit for number of sp_head objects that can be stored
  in the sp_cache for one connection.
*/
ulong stored_program_cache_size= 0;

ulong opt_slave_parallel_threads= 0;
ulong opt_slave_domain_parallel_threads= 0;
ulong opt_slave_parallel_mode;
ulong opt_binlog_commit_wait_count= 0;
ulong opt_binlog_commit_wait_usec= 0;
ulong opt_slave_parallel_max_queued= 131072;
my_bool opt_gtid_ignore_duplicates= FALSE;
uint opt_gtid_cleanup_batch_size= 64;

const double log_10[] = {
  1e000, 1e001, 1e002, 1e003, 1e004, 1e005, 1e006, 1e007, 1e008, 1e009,
  1e010, 1e011, 1e012, 1e013, 1e014, 1e015, 1e016, 1e017, 1e018, 1e019,
  1e020, 1e021, 1e022, 1e023, 1e024, 1e025, 1e026, 1e027, 1e028, 1e029,
  1e030, 1e031, 1e032, 1e033, 1e034, 1e035, 1e036, 1e037, 1e038, 1e039,
  1e040, 1e041, 1e042, 1e043, 1e044, 1e045, 1e046, 1e047, 1e048, 1e049,
  1e050, 1e051, 1e052, 1e053, 1e054, 1e055, 1e056, 1e057, 1e058, 1e059,
  1e060, 1e061, 1e062, 1e063, 1e064, 1e065, 1e066, 1e067, 1e068, 1e069,
  1e070, 1e071, 1e072, 1e073, 1e074, 1e075, 1e076, 1e077, 1e078, 1e079,
  1e080, 1e081, 1e082, 1e083, 1e084, 1e085, 1e086, 1e087, 1e088, 1e089,
  1e090, 1e091, 1e092, 1e093, 1e094, 1e095, 1e096, 1e097, 1e098, 1e099,
  1e100, 1e101, 1e102, 1e103, 1e104, 1e105, 1e106, 1e107, 1e108, 1e109,
  1e110, 1e111, 1e112, 1e113, 1e114, 1e115, 1e116, 1e117, 1e118, 1e119,
  1e120, 1e121, 1e122, 1e123, 1e124, 1e125, 1e126, 1e127, 1e128, 1e129,
  1e130, 1e131, 1e132, 1e133, 1e134, 1e135, 1e136, 1e137, 1e138, 1e139,
  1e140, 1e141, 1e142, 1e143, 1e144, 1e145, 1e146, 1e147, 1e148, 1e149,
  1e150, 1e151, 1e152, 1e153, 1e154, 1e155, 1e156, 1e157, 1e158, 1e159,
  1e160, 1e161, 1e162, 1e163, 1e164, 1e165, 1e166, 1e167, 1e168, 1e169,
  1e170, 1e171, 1e172, 1e173, 1e174, 1e175, 1e176, 1e177, 1e178, 1e179,
  1e180, 1e181, 1e182, 1e183, 1e184, 1e185, 1e186, 1e187, 1e188, 1e189,
  1e190, 1e191, 1e192, 1e193, 1e194, 1e195, 1e196, 1e197, 1e198, 1e199,
  1e200, 1e201, 1e202, 1e203, 1e204, 1e205, 1e206, 1e207, 1e208, 1e209,
  1e210, 1e211, 1e212, 1e213, 1e214, 1e215, 1e216, 1e217, 1e218, 1e219,
  1e220, 1e221, 1e222, 1e223, 1e224, 1e225, 1e226, 1e227, 1e228, 1e229,
  1e230, 1e231, 1e232, 1e233, 1e234, 1e235, 1e236, 1e237, 1e238, 1e239,
  1e240, 1e241, 1e242, 1e243, 1e244, 1e245, 1e246, 1e247, 1e248, 1e249,
  1e250, 1e251, 1e252, 1e253, 1e254, 1e255, 1e256, 1e257, 1e258, 1e259,
  1e260, 1e261, 1e262, 1e263, 1e264, 1e265, 1e266, 1e267, 1e268, 1e269,
  1e270, 1e271, 1e272, 1e273, 1e274, 1e275, 1e276, 1e277, 1e278, 1e279,
  1e280, 1e281, 1e282, 1e283, 1e284, 1e285, 1e286, 1e287, 1e288, 1e289,
  1e290, 1e291, 1e292, 1e293, 1e294, 1e295, 1e296, 1e297, 1e298, 1e299,
  1e300, 1e301, 1e302, 1e303, 1e304, 1e305, 1e306, 1e307, 1e308
};

time_t server_start_time, flush_status_time;

char mysql_home[FN_REFLEN], pidfile_name[FN_REFLEN], system_time_zone[30];
char *default_tz_name;
char log_error_file[FN_REFLEN], glob_hostname[FN_REFLEN], *opt_log_basename;
char mysql_real_data_home[FN_REFLEN],
     lc_messages_dir[FN_REFLEN], reg_ext[FN_EXTLEN],
     mysql_charsets_dir[FN_REFLEN],
     *opt_init_file, *opt_tc_log_file, *opt_ddl_recovery_file;
char *lc_messages_dir_ptr= lc_messages_dir, *log_error_file_ptr;
char mysql_unpacked_real_data_home[FN_REFLEN];
size_t mysql_unpacked_real_data_home_len;
uint mysql_real_data_home_len, mysql_data_home_len= 1;
uint reg_ext_length;
const key_map key_map_empty(0);
key_map key_map_full(0);                        // Will be initialized later

DATE_TIME_FORMAT global_date_format, global_datetime_format, global_time_format;
Time_zone *default_tz;

const char *mysql_real_data_home_ptr= mysql_real_data_home;
extern "C" {
char server_version[SERVER_VERSION_LENGTH];
}
char *server_version_ptr;
bool using_custom_server_version= false;
char *mysqld_unix_port, *opt_mysql_tmpdir;
ulong thread_handling;

my_bool encrypt_binlog;
my_bool encrypt_tmp_disk_tables, encrypt_tmp_files;

/** name of reference on left expression in rewritten IN subquery */
const LEX_CSTRING in_left_expr_name= {STRING_WITH_LEN("<left expr>") };
/** name of additional condition */
const LEX_CSTRING in_having_cond= {STRING_WITH_LEN("<IN HAVING>") };
const LEX_CSTRING in_additional_cond= {STRING_WITH_LEN("<IN COND>") };

/** Number of connection errors when selecting on the listening port */
ulong connection_errors_select= 0;
/** Number of connection errors when accepting sockets in the listening port. */
ulong connection_errors_accept= 0;
/** Number of connection errors from TCP wrappers. */
ulong connection_errors_tcpwrap= 0;
/** Number of connection errors from internal server errors. */
ulong connection_errors_internal= 0;
/** Number of connection errors from the server max_connection limit. */
ulong connection_errors_max_connection= 0;
/** Number of errors when reading the peer address. */
ulong connection_errors_peer_addr= 0;

/* classes for comparation parsing/processing */
Eq_creator eq_creator;
Ne_creator ne_creator;
Gt_creator gt_creator;
Lt_creator lt_creator;
Ge_creator ge_creator;
Le_creator le_creator;

THD_list server_threads;
Rpl_filter* cur_rpl_filter;
Rpl_filter* global_rpl_filter;
Rpl_filter* binlog_filter;

struct system_variables global_system_variables;
/**
  Following is just for options parsing, used with a difference against
  global_system_variables.

  TODO: something should be done to get rid of following variables
*/
const char *current_dbug_option="";

struct system_variables max_system_variables;
struct system_status_var global_status_var;

MY_TMPDIR mysql_tmpdir_list;
static MY_BITMAP temp_pool;
static mysql_mutex_t LOCK_temp_pool;

void temp_pool_clear_bit(uint bit)
{
  mysql_mutex_lock(&LOCK_temp_pool);
  bitmap_clear_bit(&temp_pool, bit);
  mysql_mutex_unlock(&LOCK_temp_pool);
}

uint temp_pool_set_next()
{
  mysql_mutex_lock(&LOCK_temp_pool);
  uint res= bitmap_set_next(&temp_pool);
  mysql_mutex_unlock(&LOCK_temp_pool);
  return res;
}

CHARSET_INFO *system_charset_info, *files_charset_info ;
CHARSET_INFO *national_charset_info, *table_alias_charset;
CHARSET_INFO *character_set_filesystem;
CHARSET_INFO *error_message_charset_info;

MY_LOCALE *my_default_lc_messages;
MY_LOCALE *my_default_lc_time_names;

SHOW_COMP_OPTION have_ssl, have_symlink, have_dlopen, have_query_cache;
SHOW_COMP_OPTION have_geometry, have_rtree_keys;
SHOW_COMP_OPTION have_crypt, have_compress;
SHOW_COMP_OPTION have_profiling;
SHOW_COMP_OPTION have_openssl;

#ifndef EMBEDDED_LIBRARY
static std::atomic<char*> shutdown_user;
#endif //EMBEDDED_LIBRARY
std::atomic<my_thread_id> shutdown_thread_id;

/* Thread specific variables */

static thread_local THD *THR_THD;

/**
  Get current THD object from thread local data

  @retval     The THD object for the thread, NULL if not connection thread
*/

MYSQL_THD _current_thd() { return THR_THD; }
void set_current_thd(THD *thd) { THR_THD= thd; }

/*
  LOCK_start_thread is used to syncronize thread start and stop with
  other threads.

  It also protects these variables:
  select_thread_in_use
  slave_init_thread_running
  check_temp_dir() call
*/
mysql_mutex_t  LOCK_start_thread;

mysql_mutex_t
  LOCK_status, LOCK_error_log, LOCK_short_uuid_generator,
  LOCK_delayed_insert, LOCK_delayed_status, LOCK_delayed_create,
  LOCK_crypt,
  LOCK_global_system_variables,
  LOCK_user_conn,
  LOCK_error_messages;
mysql_mutex_t LOCK_stats, LOCK_global_user_client_stats,
              LOCK_global_table_stats, LOCK_global_index_stats;

/* This protects against changes in master_info_index */
mysql_mutex_t LOCK_active_mi;

/* This protects connection id.*/
mysql_mutex_t LOCK_thread_id;

/**
  The below lock protects access to two global server variables:
  max_prepared_stmt_count and prepared_stmt_count. These variables
  set the limit and hold the current total number of prepared statements
  in the server, respectively. As PREPARE/DEALLOCATE rate in a loaded
  server may be fairly high, we need a dedicated lock.
*/
mysql_mutex_t LOCK_prepared_stmt_count;
#ifdef HAVE_OPENSSL
mysql_mutex_t LOCK_des_key_file;
#endif
mysql_mutex_t LOCK_backup_log;
mysql_rwlock_t LOCK_grant, LOCK_sys_init_connect, LOCK_sys_init_slave;
mysql_rwlock_t LOCK_ssl_refresh;
mysql_rwlock_t LOCK_all_status_vars;
mysql_prlock_t LOCK_system_variables_hash;
mysql_cond_t COND_start_thread;
pthread_t signal_thread;
pthread_attr_t connection_attrib;
mysql_mutex_t LOCK_server_started;
mysql_cond_t COND_server_started;

int mysqld_server_started=0, mysqld_server_initialized= 0;
File_parser_dummy_hook file_parser_dummy_hook;

/* replication parameters, if master_host is not NULL, we are a slave */
uint report_port= 0;
ulong master_retry_count=0;
char *master_info_file;
char *relay_log_info_file, *report_user, *report_password, *report_host;
char *opt_relay_logname = 0, *opt_relaylog_index_name=0;
char *opt_logname, *opt_slow_logname, *opt_bin_logname;
char *opt_binlog_index_name=0;



/* Static variables */

my_bool opt_stack_trace;
my_bool opt_expect_abort= 0, opt_bootstrap= 0;
static my_bool opt_myisam_log;
static int cleanup_done;
static ulong opt_specialflag;
char *mysql_home_ptr, *pidfile_name_ptr;
/** Initial command line arguments (count), after load_defaults().*/
static int defaults_argc;
/**
  Initial command line arguments (arguments), after load_defaults().
  This memory is allocated by @c load_defaults() and should be freed
  using @c free_defaults().
  Do not modify defaults_argc / defaults_argv,
  use remaining_argc / remaining_argv instead to parse the command
  line arguments in multiple steps.
*/
static char **defaults_argv;
/** Remaining command line arguments (count), filtered by handle_options().*/
static int remaining_argc;
/** Remaining command line arguments (arguments), filtered by handle_options().*/
static char **remaining_argv;

int orig_argc;
char **orig_argv;

static struct my_option pfs_early_options[]=
{
#ifdef WITH_PERFSCHEMA_STORAGE_ENGINE
  {"performance_schema_instrument", OPT_PFS_INSTRUMENT,
    "Default startup value for a performance schema instrument.",
    &pfs_param.m_pfs_instrument, &pfs_param.m_pfs_instrument, 0, GET_STR,
    OPT_ARG, 0, 0, 0, 0, 0, 0},
  {"performance_schema_consumer_events_stages_current", 0,
    "Default startup value for the events_stages_current consumer.",
    &pfs_param.m_consumer_events_stages_current_enabled,
    &pfs_param.m_consumer_events_stages_current_enabled, 0, GET_BOOL,
    OPT_ARG, FALSE, 0, 0, 0, 0, 0},
  {"performance_schema_consumer_events_stages_history", 0,
    "Default startup value for the events_stages_history consumer.",
    &pfs_param.m_consumer_events_stages_history_enabled,
    &pfs_param.m_consumer_events_stages_history_enabled, 0,
    GET_BOOL, OPT_ARG, FALSE, 0, 0, 0, 0, 0},
  {"performance_schema_consumer_events_stages_history_long", 0,
    "Default startup value for the events_stages_history_long consumer.",
    &pfs_param.m_consumer_events_stages_history_long_enabled,
    &pfs_param.m_consumer_events_stages_history_long_enabled, 0,
    GET_BOOL, OPT_ARG, FALSE, 0, 0, 0, 0, 0},
  {"performance_schema_consumer_events_statements_current", 0,
    "Default startup value for the events_statements_current consumer.",
    &pfs_param.m_consumer_events_statements_current_enabled,
    &pfs_param.m_consumer_events_statements_current_enabled, 0, GET_BOOL,
    OPT_ARG, FALSE, 0, 0, 0, 0, 0},
  {"performance_schema_consumer_events_statements_history", 0,
    "Default startup value for the events_statements_history consumer.",
    &pfs_param.m_consumer_events_statements_history_enabled,
    &pfs_param.m_consumer_events_statements_history_enabled, 0,
    GET_BOOL, OPT_ARG, FALSE, 0, 0, 0, 0, 0},
  {"performance_schema_consumer_events_statements_history_long", 0,
    "Default startup value for the events_statements_history_long consumer.",
    &pfs_param.m_consumer_events_statements_history_long_enabled,
    &pfs_param.m_consumer_events_statements_history_long_enabled, 0,
    GET_BOOL, OPT_ARG, FALSE, 0, 0, 0, 0, 0},
  {"performance_schema_consumer_events_transactions_current", 0,
    "Default startup value for the events_transactions_current consumer.",
    &pfs_param.m_consumer_events_transactions_current_enabled,
    &pfs_param.m_consumer_events_transactions_current_enabled, 0,
    GET_BOOL, OPT_ARG, FALSE, 0, 0, 0, 0, 0},
  {"performance_schema_consumer_events_transactions_history", 0,
    "Default startup value for the events_transactions_history consumer.",
    &pfs_param.m_consumer_events_transactions_history_enabled,
    &pfs_param.m_consumer_events_transactions_history_enabled, 0,
    GET_BOOL, OPT_ARG, FALSE, 0, 0, 0, 0, 0},
  {"performance_schema_consumer_events_transactions_history_long", 0,
    "Default startup value for the events_transactions_history_long consumer.",
    &pfs_param.m_consumer_events_transactions_history_long_enabled,
    &pfs_param.m_consumer_events_transactions_history_long_enabled, 0,
    GET_BOOL, OPT_ARG, FALSE, 0, 0, 0, 0, 0},
  {"performance_schema_consumer_events_waits_current", 0,
    "Default startup value for the events_waits_current consumer.",
    &pfs_param.m_consumer_events_waits_current_enabled,
    &pfs_param.m_consumer_events_waits_current_enabled, 0,
    GET_BOOL, OPT_ARG, FALSE, 0, 0, 0, 0, 0},
  {"performance_schema_consumer_events_waits_history", 0,
    "Default startup value for the events_waits_history consumer.",
    &pfs_param.m_consumer_events_waits_history_enabled,
    &pfs_param.m_consumer_events_waits_history_enabled, 0,
    GET_BOOL, OPT_ARG, FALSE, 0, 0, 0, 0, 0},
  {"performance_schema_consumer_events_waits_history_long", 0,
    "Default startup value for the events_waits_history_long consumer.",
    &pfs_param.m_consumer_events_waits_history_long_enabled,
    &pfs_param.m_consumer_events_waits_history_long_enabled, 0,
    GET_BOOL, OPT_ARG, FALSE, 0, 0, 0, 0, 0},
  {"performance_schema_consumer_global_instrumentation", 0,
    "Default startup value for the global_instrumentation consumer.",
    &pfs_param.m_consumer_global_instrumentation_enabled,
    &pfs_param.m_consumer_global_instrumentation_enabled, 0,
    GET_BOOL, OPT_ARG, TRUE, 0, 0, 0, 0, 0},
  {"performance_schema_consumer_thread_instrumentation", 0,
    "Default startup value for the thread_instrumentation consumer.",
    &pfs_param.m_consumer_thread_instrumentation_enabled,
    &pfs_param.m_consumer_thread_instrumentation_enabled, 0,
    GET_BOOL, OPT_ARG, TRUE, 0, 0, 0, 0, 0},
  {"performance_schema_consumer_statements_digest", 0,
    "Default startup value for the statements_digest consumer.",
    &pfs_param.m_consumer_statement_digest_enabled,
    &pfs_param.m_consumer_statement_digest_enabled, 0,
    GET_BOOL, OPT_ARG, TRUE, 0, 0, 0, 0, 0},
#endif /* WITH_PERFSCHEMA_STORAGE_ENGINE */
  {"getopt-prefix-matching", 0,
    "Recognize command-line options by their unambiguos prefixes.",
    &my_getopt_prefix_matching, &my_getopt_prefix_matching, 0, GET_BOOL,
    NO_ARG, 1, 0, 1, 0, 0, 0}
};

PSI_file_key key_file_binlog,  key_file_binlog_cache, key_file_binlog_index,
  key_file_binlog_index_cache, key_file_casetest,
  key_file_dbopt, key_file_des_key_file, key_file_ERRMSG, key_select_to_file,
  key_file_fileparser, key_file_frm, key_file_global_ddl_log, key_file_load,
  key_file_loadfile, key_file_log_event_data, key_file_log_event_info,
  key_file_log_ddl,
  key_file_master_info, key_file_misc, key_file_partition_ddl_log,
  key_file_pid, key_file_relay_log_info, key_file_send_file, key_file_tclog,
  key_file_trg, key_file_trn, key_file_init;
PSI_file_key key_file_query_log, key_file_slow_log;
PSI_file_key key_file_relaylog, key_file_relaylog_index,
             key_file_relaylog_cache, key_file_relaylog_index_cache;
PSI_file_key key_file_binlog_state;

#ifdef HAVE_PSI_INTERFACE
#ifdef HAVE_MMAP
PSI_mutex_key key_PAGE_lock, key_LOCK_sync, key_LOCK_active, key_LOCK_pool,
  key_LOCK_pending_checkpoint;
#endif /* HAVE_MMAP */

#ifdef HAVE_OPENSSL
PSI_mutex_key key_LOCK_des_key_file;
#endif /* HAVE_OPENSSL */

PSI_mutex_key key_BINLOG_LOCK_index, key_BINLOG_LOCK_xid_list,
  key_BINLOG_LOCK_binlog_background_thread,
  key_LOCK_binlog_end_pos,
  key_delayed_insert_mutex, key_hash_filo_lock, key_LOCK_active_mi,
  key_LOCK_crypt, key_LOCK_delayed_create,
  key_LOCK_delayed_insert, key_LOCK_delayed_status, key_LOCK_error_log,
  key_LOCK_gdl, key_LOCK_global_system_variables,
  key_LOCK_manager, key_LOCK_backup_log,
  key_LOCK_prepared_stmt_count,
  key_LOCK_rpl_status, key_LOCK_server_started,
  key_LOCK_status, key_LOCK_temp_pool,
  key_LOCK_system_variables_hash, key_LOCK_thd_data, key_LOCK_thd_kill,
  key_LOCK_user_conn, key_LOCK_uuid_short_generator, key_LOG_LOCK_log,
  key_master_info_data_lock, key_master_info_run_lock,
  key_master_info_sleep_lock, key_master_info_start_stop_lock,
  key_master_info_start_alter_lock,
  key_master_info_start_alter_list_lock,
  key_mutex_slave_reporting_capability_err_lock, key_relay_log_info_data_lock,
  key_rpl_group_info_sleep_lock,
  key_relay_log_info_log_space_lock, key_relay_log_info_run_lock,
  key_structure_guard_mutex, key_TABLE_SHARE_LOCK_ha_data,
  key_LOCK_error_messages,
  key_LOCK_start_thread,
  key_PARTITION_LOCK_auto_inc;
PSI_mutex_key key_RELAYLOG_LOCK_index;
PSI_mutex_key key_LOCK_relaylog_end_pos;
PSI_mutex_key key_LOCK_thread_id;
PSI_mutex_key key_LOCK_slave_state, key_LOCK_binlog_state,
  key_LOCK_rpl_thread, key_LOCK_rpl_thread_pool, key_LOCK_parallel_entry;
PSI_mutex_key key_LOCK_rpl_semi_sync_master_enabled;
PSI_mutex_key key_LOCK_binlog;

PSI_mutex_key key_LOCK_stats,
  key_LOCK_global_user_client_stats, key_LOCK_global_table_stats,
  key_LOCK_global_index_stats,
  key_LOCK_wakeup_ready, key_LOCK_wait_commit;
PSI_mutex_key key_LOCK_gtid_waiting;

PSI_mutex_key key_LOCK_after_binlog_sync;
PSI_mutex_key key_LOCK_prepare_ordered, key_LOCK_commit_ordered;
PSI_mutex_key key_TABLE_SHARE_LOCK_share;
PSI_mutex_key key_LOCK_ack_receiver;

PSI_mutex_key key_TABLE_SHARE_LOCK_rotation;
PSI_cond_key key_TABLE_SHARE_COND_rotation;

static PSI_mutex_info all_server_mutexes[]=
{
#ifdef HAVE_MMAP
  { &key_PAGE_lock, "PAGE::lock", 0},
  { &key_LOCK_sync, "TC_LOG_MMAP::LOCK_sync", 0},
  { &key_LOCK_active, "TC_LOG_MMAP::LOCK_active", 0},
  { &key_LOCK_pool, "TC_LOG_MMAP::LOCK_pool", 0},
  { &key_LOCK_pool, "TC_LOG_MMAP::LOCK_pending_checkpoint", 0},
#endif /* HAVE_MMAP */

#ifdef HAVE_OPENSSL
  { &key_LOCK_des_key_file, "LOCK_des_key_file", PSI_FLAG_GLOBAL},
#endif /* HAVE_OPENSSL */

  { &key_BINLOG_LOCK_index, "MYSQL_BIN_LOG::LOCK_index", 0},
  { &key_BINLOG_LOCK_xid_list, "MYSQL_BIN_LOG::LOCK_xid_list", 0},
  { &key_BINLOG_LOCK_binlog_background_thread, "MYSQL_BIN_LOG::LOCK_binlog_background_thread", 0},
  { &key_LOCK_binlog_end_pos, "MYSQL_BIN_LOG::LOCK_binlog_end_pos", 0 },
  { &key_RELAYLOG_LOCK_index, "MYSQL_RELAY_LOG::LOCK_index", 0},
  { &key_LOCK_relaylog_end_pos, "MYSQL_RELAY_LOG::LOCK_binlog_end_pos", 0},
  { &key_delayed_insert_mutex, "Delayed_insert::mutex", 0},
  { &key_hash_filo_lock, "hash_filo::lock", 0},
  { &key_LOCK_active_mi, "LOCK_active_mi", PSI_FLAG_GLOBAL},
  { &key_LOCK_backup_log, "LOCK_backup_log", PSI_FLAG_GLOBAL},
  { &key_LOCK_temp_pool, "LOCK_temp_pool", PSI_FLAG_GLOBAL},
  { &key_LOCK_thread_id, "LOCK_thread_id", PSI_FLAG_GLOBAL},
  { &key_LOCK_crypt, "LOCK_crypt", PSI_FLAG_GLOBAL},
  { &key_LOCK_delayed_create, "LOCK_delayed_create", PSI_FLAG_GLOBAL},
  { &key_LOCK_delayed_insert, "LOCK_delayed_insert", PSI_FLAG_GLOBAL},
  { &key_LOCK_delayed_status, "LOCK_delayed_status", PSI_FLAG_GLOBAL},
  { &key_LOCK_error_log, "LOCK_error_log", PSI_FLAG_GLOBAL},
  { &key_LOCK_gdl, "LOCK_gdl", PSI_FLAG_GLOBAL},
  { &key_LOCK_global_system_variables, "LOCK_global_system_variables", PSI_FLAG_GLOBAL},
  { &key_LOCK_manager, "LOCK_manager", PSI_FLAG_GLOBAL},
  { &key_LOCK_prepared_stmt_count, "LOCK_prepared_stmt_count", PSI_FLAG_GLOBAL},
  { &key_LOCK_rpl_status, "LOCK_rpl_status", PSI_FLAG_GLOBAL},
  { &key_LOCK_server_started, "LOCK_server_started", PSI_FLAG_GLOBAL},
  { &key_LOCK_status, "LOCK_status", PSI_FLAG_GLOBAL},
  { &key_LOCK_system_variables_hash, "LOCK_system_variables_hash", PSI_FLAG_GLOBAL},
  { &key_LOCK_stats, "LOCK_stats", PSI_FLAG_GLOBAL},
  { &key_LOCK_global_user_client_stats, "LOCK_global_user_client_stats", PSI_FLAG_GLOBAL},
  { &key_LOCK_global_table_stats, "LOCK_global_table_stats", PSI_FLAG_GLOBAL},
  { &key_LOCK_global_index_stats, "LOCK_global_index_stats", PSI_FLAG_GLOBAL},
  { &key_LOCK_wakeup_ready, "THD::LOCK_wakeup_ready", 0},
  { &key_LOCK_wait_commit, "wait_for_commit::LOCK_wait_commit", 0},
  { &key_LOCK_gtid_waiting, "gtid_waiting::LOCK_gtid_waiting", 0},
  { &key_LOCK_thd_data, "THD::LOCK_thd_data", 0},
  { &key_LOCK_thd_kill, "THD::LOCK_thd_kill", 0},
  { &key_LOCK_user_conn, "LOCK_user_conn", PSI_FLAG_GLOBAL},
  { &key_LOCK_uuid_short_generator, "LOCK_uuid_short_generator", PSI_FLAG_GLOBAL},
  { &key_LOG_LOCK_log, "LOG::LOCK_log", 0},
  { &key_master_info_data_lock, "Master_info::data_lock", 0},
  { &key_master_info_start_stop_lock, "Master_info::start_stop_lock", 0},
  { &key_master_info_run_lock, "Master_info::run_lock", 0},
  { &key_master_info_sleep_lock, "Master_info::sleep_lock", 0},
  { &key_master_info_start_alter_lock, "Master_info::start_alter_lock", 0},
  { &key_master_info_start_alter_list_lock, "Master_info::start_alter_lock", 0},
  { &key_mutex_slave_reporting_capability_err_lock, "Slave_reporting_capability::err_lock", 0},
  { &key_relay_log_info_data_lock, "Relay_log_info::data_lock", 0},
  { &key_relay_log_info_log_space_lock, "Relay_log_info::log_space_lock", 0},
  { &key_relay_log_info_run_lock, "Relay_log_info::run_lock", 0},
  { &key_rpl_group_info_sleep_lock, "Rpl_group_info::sleep_lock", 0},
  { &key_structure_guard_mutex, "Query_cache::structure_guard_mutex", 0},
  { &key_TABLE_SHARE_LOCK_ha_data, "TABLE_SHARE::LOCK_ha_data", 0},
  { &key_TABLE_SHARE_LOCK_share, "TABLE_SHARE::LOCK_share", 0},
  { &key_TABLE_SHARE_LOCK_rotation, "TABLE_SHARE::LOCK_rotation", 0},
  { &key_LOCK_error_messages, "LOCK_error_messages", PSI_FLAG_GLOBAL},
  { &key_LOCK_prepare_ordered, "LOCK_prepare_ordered", PSI_FLAG_GLOBAL},
  { &key_LOCK_after_binlog_sync, "LOCK_after_binlog_sync", PSI_FLAG_GLOBAL},
  { &key_LOCK_commit_ordered, "LOCK_commit_ordered", PSI_FLAG_GLOBAL},
  { &key_PARTITION_LOCK_auto_inc, "HA_DATA_PARTITION::LOCK_auto_inc", 0},
  { &key_LOCK_slave_state, "LOCK_slave_state", 0},
  { &key_LOCK_start_thread, "LOCK_start_thread", PSI_FLAG_GLOBAL},
  { &key_LOCK_binlog_state, "LOCK_binlog_state", 0},
  { &key_LOCK_rpl_thread, "LOCK_rpl_thread", 0},
  { &key_LOCK_rpl_thread_pool, "LOCK_rpl_thread_pool", 0},
  { &key_LOCK_parallel_entry, "LOCK_parallel_entry", 0},
  { &key_LOCK_ack_receiver, "Ack_receiver::mutex", 0},
  { &key_LOCK_rpl_semi_sync_master_enabled, "LOCK_rpl_semi_sync_master_enabled", 0},
  { &key_LOCK_binlog, "LOCK_binlog", 0}
};

PSI_rwlock_key key_rwlock_LOCK_grant, key_rwlock_LOCK_logger,
  key_rwlock_LOCK_sys_init_connect, key_rwlock_LOCK_sys_init_slave,
  key_rwlock_LOCK_system_variables_hash, key_rwlock_query_cache_query_lock,
  key_LOCK_SEQUENCE,
  key_rwlock_LOCK_vers_stats, key_rwlock_LOCK_stat_serial,
  key_rwlock_LOCK_ssl_refresh,
  key_rwlock_THD_list,
  key_rwlock_LOCK_all_status_vars;

static PSI_rwlock_info all_server_rwlocks[]=
{
#ifdef HAVE_OPENSSL10
  { &key_rwlock_openssl, "CRYPTO_dynlock_value::lock", 0},
#endif
  { &key_rwlock_LOCK_grant, "LOCK_grant", PSI_FLAG_GLOBAL},
  { &key_rwlock_LOCK_logger, "LOGGER::LOCK_logger", 0},
  { &key_rwlock_LOCK_sys_init_connect, "LOCK_sys_init_connect", PSI_FLAG_GLOBAL},
  { &key_rwlock_LOCK_sys_init_slave, "LOCK_sys_init_slave", PSI_FLAG_GLOBAL},
  { &key_LOCK_SEQUENCE, "LOCK_SEQUENCE", 0},
  { &key_rwlock_LOCK_system_variables_hash, "LOCK_system_variables_hash", PSI_FLAG_GLOBAL},
  { &key_rwlock_query_cache_query_lock, "Query_cache_query::lock", 0},
  { &key_rwlock_LOCK_vers_stats, "Vers_field_stats::lock", 0},
  { &key_rwlock_LOCK_stat_serial, "TABLE_SHARE::LOCK_stat_serial", 0},
  { &key_rwlock_LOCK_ssl_refresh, "LOCK_ssl_refresh", PSI_FLAG_GLOBAL },
  { &key_rwlock_THD_list, "THD_list::lock", PSI_FLAG_GLOBAL },
  { &key_rwlock_LOCK_all_status_vars, "LOCK_all_status_vars", PSI_FLAG_GLOBAL }
};

#ifdef HAVE_MMAP
PSI_cond_key key_PAGE_cond, key_COND_active, key_COND_pool;
#endif /* HAVE_MMAP */

PSI_cond_key key_BINLOG_COND_xid_list,
  key_BINLOG_COND_bin_log_updated, key_BINLOG_COND_relay_log_updated,
  key_BINLOG_COND_binlog_background_thread,
  key_BINLOG_COND_binlog_background_thread_end,
  key_COND_cache_status_changed, key_COND_manager,
  key_COND_rpl_status, key_COND_server_started,
  key_delayed_insert_cond, key_delayed_insert_cond_client,
  key_item_func_sleep_cond, key_master_info_data_cond,
  key_master_info_start_cond, key_master_info_stop_cond,
  key_master_info_sleep_cond,
  key_relay_log_info_data_cond, key_relay_log_info_log_space_cond,
  key_relay_log_info_start_cond, key_relay_log_info_stop_cond,
  key_rpl_group_info_sleep_cond,
  key_TABLE_SHARE_cond, key_user_level_lock_cond,
  key_COND_start_thread, key_COND_binlog_send,
  key_BINLOG_COND_queue_busy;
PSI_cond_key key_RELAYLOG_COND_relay_log_updated,
  key_RELAYLOG_COND_bin_log_updated, key_COND_wakeup_ready,
  key_COND_wait_commit;
PSI_cond_key key_RELAYLOG_COND_queue_busy;
PSI_cond_key key_TC_LOG_MMAP_COND_queue_busy;
PSI_cond_key key_COND_rpl_thread_queue, key_COND_rpl_thread,
  key_COND_rpl_thread_stop, key_COND_rpl_thread_pool,
  key_COND_parallel_entry, key_COND_group_commit_orderer,
  key_COND_prepare_ordered;
PSI_cond_key key_COND_wait_gtid, key_COND_gtid_ignore_duplicates;
PSI_cond_key key_COND_ack_receiver;

static PSI_cond_info all_server_conds[]=
{
#ifdef HAVE_MMAP
  { &key_PAGE_cond, "PAGE::cond", 0},
  { &key_COND_active, "TC_LOG_MMAP::COND_active", 0},
  { &key_COND_pool, "TC_LOG_MMAP::COND_pool", 0},
  { &key_TC_LOG_MMAP_COND_queue_busy, "TC_LOG_MMAP::COND_queue_busy", 0},
#endif /* HAVE_MMAP */
  { &key_BINLOG_COND_bin_log_updated, "MYSQL_BIN_LOG::COND_bin_log_updated", 0}, { &key_BINLOG_COND_relay_log_updated, "MYSQL_BIN_LOG::COND_relay_log_updated", 0},
  { &key_BINLOG_COND_xid_list, "MYSQL_BIN_LOG::COND_xid_list", 0},
  { &key_BINLOG_COND_binlog_background_thread, "MYSQL_BIN_LOG::COND_binlog_background_thread", 0},
  { &key_BINLOG_COND_binlog_background_thread_end, "MYSQL_BIN_LOG::COND_binlog_background_thread_end", 0},
  { &key_BINLOG_COND_queue_busy, "MYSQL_BIN_LOG::COND_queue_busy", 0},
  { &key_RELAYLOG_COND_relay_log_updated, "MYSQL_RELAY_LOG::COND_relay_log_updated", 0},
  { &key_RELAYLOG_COND_bin_log_updated, "MYSQL_RELAY_LOG::COND_bin_log_updated", 0},
  { &key_RELAYLOG_COND_queue_busy, "MYSQL_RELAY_LOG::COND_queue_busy", 0},
  { &key_COND_wakeup_ready, "THD::COND_wakeup_ready", 0},
  { &key_COND_wait_commit, "wait_for_commit::COND_wait_commit", 0},
  { &key_COND_cache_status_changed, "Query_cache::COND_cache_status_changed", 0},
  { &key_COND_manager, "COND_manager", PSI_FLAG_GLOBAL},
  { &key_COND_server_started, "COND_server_started", PSI_FLAG_GLOBAL},
  { &key_delayed_insert_cond, "Delayed_insert::cond", 0},
  { &key_delayed_insert_cond_client, "Delayed_insert::cond_client", 0},
  { &key_item_func_sleep_cond, "Item_func_sleep::cond", 0},
  { &key_master_info_data_cond, "Master_info::data_cond", 0},
  { &key_master_info_start_cond, "Master_info::start_cond", 0},
  { &key_master_info_stop_cond, "Master_info::stop_cond", 0},
  { &key_master_info_sleep_cond, "Master_info::sleep_cond", 0},
  { &key_relay_log_info_data_cond, "Relay_log_info::data_cond", 0},
  { &key_relay_log_info_log_space_cond, "Relay_log_info::log_space_cond", 0},
  { &key_relay_log_info_start_cond, "Relay_log_info::start_cond", 0},
  { &key_relay_log_info_stop_cond, "Relay_log_info::stop_cond", 0},
  { &key_rpl_group_info_sleep_cond, "Rpl_group_info::sleep_cond", 0},
  { &key_TABLE_SHARE_cond, "TABLE_SHARE::cond", 0},
  { &key_user_level_lock_cond, "User_level_lock::cond", 0},
  { &key_COND_rpl_thread, "COND_rpl_thread", 0},
  { &key_COND_rpl_thread_queue, "COND_rpl_thread_queue", 0},
  { &key_COND_rpl_thread_stop, "COND_rpl_thread_stop", 0},
  { &key_COND_rpl_thread_pool, "COND_rpl_thread_pool", 0},
  { &key_COND_parallel_entry, "COND_parallel_entry", 0},
  { &key_COND_group_commit_orderer, "COND_group_commit_orderer", 0},
  { &key_COND_prepare_ordered, "COND_prepare_ordered", 0},
  { &key_COND_start_thread, "COND_start_thread", PSI_FLAG_GLOBAL},
  { &key_COND_wait_gtid, "COND_wait_gtid", 0},
  { &key_COND_gtid_ignore_duplicates, "COND_gtid_ignore_duplicates", 0},
  { &key_COND_ack_receiver, "Ack_receiver::cond", 0},
  { &key_COND_binlog_send, "COND_binlog_send", 0},
  { &key_TABLE_SHARE_COND_rotation, "TABLE_SHARE::COND_rotation", 0}
};

PSI_thread_key key_thread_delayed_insert,
  key_thread_handle_manager, key_thread_main,
  key_thread_one_connection, key_thread_signal_hand,
  key_thread_slave_background, key_rpl_parallel_thread;
PSI_thread_key key_thread_ack_receiver;

static PSI_thread_info all_server_threads[]=
{
  { &key_thread_delayed_insert, "delayed_insert", 0},
  { &key_thread_handle_manager, "manager", PSI_FLAG_GLOBAL},
  { &key_thread_main, "main", PSI_FLAG_GLOBAL},
  { &key_thread_one_connection, "one_connection", 0},
  { &key_thread_signal_hand, "signal_handler", PSI_FLAG_GLOBAL},
  { &key_thread_slave_background, "slave_background", PSI_FLAG_GLOBAL},
  { &key_thread_ack_receiver, "Ack_receiver", PSI_FLAG_GLOBAL},
  { &key_rpl_parallel_thread, "rpl_parallel_thread", 0}
};

#ifdef HAVE_MMAP
PSI_file_key key_file_map;
#endif /* HAVE_MMAP */

#endif /* HAVE_PSI_INTERFACE */

#ifdef HAVE_PSI_STATEMENT_INTERFACE
PSI_statement_info stmt_info_new_packet;
#endif

#ifndef EMBEDDED_LIBRARY
void net_before_header_psi(struct st_net *net, void *thd, size_t /* unused: count */)
{
  DBUG_ASSERT(thd);
  /*
    We only come where when the server is IDLE, waiting for the next command.
    Technically, it is a wait on a socket, which may take a long time,
    because the call is blocking.
    Disable the socket instrumentation, to avoid recording a SOCKET event.
    Instead, start explicitly an IDLE event.
  */
  MYSQL_SOCKET_SET_STATE(net->vio->mysql_socket, PSI_SOCKET_STATE_IDLE);
  MYSQL_START_IDLE_WAIT(static_cast<THD*>(thd)->m_idle_psi,
                        &static_cast<THD*>(thd)->m_idle_state);
}

void net_after_header_psi(struct st_net *net, void *user_data,
                          size_t /* unused: count */, my_bool rc)
{
  THD *thd;
  thd= static_cast<THD*> (user_data);
  DBUG_ASSERT(thd != NULL);

  /*
    The server just got data for a network packet header,
    from the network layer.
    The IDLE event is now complete, since we now have a message to process.
    We need to:
    - start a new STATEMENT event
    - start a new STAGE event, within this statement,
    - start recording SOCKET WAITS events, within this stage.
    The proper order is critical to get events numbered correctly,
    and nested in the proper parent.
  */
  MYSQL_END_IDLE_WAIT(thd->m_idle_psi);

  if (! rc)
  {
    thd->m_statement_psi= MYSQL_START_STATEMENT(&thd->m_statement_state,
                                                stmt_info_new_packet.m_key,
                                                thd->get_db(), thd->db.length,
                                                thd->charset(), NULL);

    THD_STAGE_INFO(thd, stage_starting);
  }

  /*
    TODO: consider recording a SOCKET event for the bytes just read,
    by also passing count here.
  */
  MYSQL_SOCKET_SET_STATE(net->vio->mysql_socket, PSI_SOCKET_STATE_ACTIVE);
}


void init_net_server_extension(THD *thd)
{
  /* Start with a clean state for connection events. */
  thd->m_idle_psi= NULL;
  thd->m_statement_psi= NULL;
  /* Hook up the NET_SERVER callback in the net layer. */
  thd->m_net_server_extension.m_user_data= thd;
  thd->m_net_server_extension.m_before_header= net_before_header_psi;
  thd->m_net_server_extension.m_after_header= net_after_header_psi;
  /* Activate this private extension for the mysqld server. */
  thd->net.extension= & thd->m_net_server_extension;
}
#else
void init_net_server_extension(THD *thd)
{
}
#endif /* EMBEDDED_LIBRARY */


/**
  A log message for the error log, buffered in memory.
  Log messages are temporarily buffered when generated before the error log
  is initialized, and then printed once the error log is ready.
*/
class Buffered_log : public Sql_alloc
{
public:
  Buffered_log(enum loglevel level, const char *message);

  ~Buffered_log()
  {}

  void print(void);

private:
  /** Log message level. */
  enum loglevel m_level;
  /** Log message text. */
  String m_message;
};

/**
  Constructor.
  @param level          the message log level
  @param message        the message text
*/
Buffered_log::Buffered_log(enum loglevel level, const char *message)
  : m_level(level), m_message()
{
  m_message.copy(message, strlen(message), &my_charset_latin1);
}

/**
  Print a buffered log to the real log file.
*/
void Buffered_log::print()
{
  /*
    Since messages are buffered, they can be printed out
    of order with other entries in the log.
    Add "Buffered xxx" to the message text to prevent confusion.
  */
  switch(m_level)
  {
  case ERROR_LEVEL:
    sql_print_error("Buffered error: %s", m_message.c_ptr_safe());
    break;
  case WARNING_LEVEL:
    sql_print_warning("Buffered warning: %s", m_message.c_ptr_safe());
    break;
  case INFORMATION_LEVEL:
    /*
      Messages printed as "information" still end up in the mysqld *error* log,
      but with a [Note] tag instead of an [ERROR] tag.
      While this is probably fine for a human reading the log,
      it is upsetting existing automated scripts used to parse logs,
      because such scripts are likely to not already handle [Note] properly.
      INFORMATION_LEVEL messages are simply silenced, on purpose,
      to avoid un needed verbosity.
    */
    break;
  }
}

/**
  Collection of all the buffered log messages.
*/
class Buffered_logs
{
public:
  Buffered_logs()
  {}

  ~Buffered_logs()
  {}

  void init();
  void cleanup();

  void buffer(enum loglevel m_level, const char *msg);
  void print();
private:
  /**
    Memory root to use to store buffered logs.
    This memory root lifespan is between init and cleanup.
    Once the buffered logs are printed, they are not needed anymore,
    and all the memory used is reclaimed.
  */
  MEM_ROOT m_root;
  /** List of buffered log messages. */
  List<Buffered_log> m_list;
};

void Buffered_logs::init()
{
  init_alloc_root(PSI_NOT_INSTRUMENTED, &m_root, 1024, 0, MYF(0));
}

void Buffered_logs::cleanup()
{
  m_list.delete_elements();
  free_root(&m_root, MYF(0));
}

/**
  Add a log message to the buffer.
*/
void Buffered_logs::buffer(enum loglevel level, const char *msg)
{
  /*
    Do not let Sql_alloc::operator new(size_t) allocate memory,
    there is no memory root associated with the main() thread.
    Give explicitly the proper memory root to use to
    Sql_alloc::operator new(size_t, MEM_ROOT *) instead.
  */
  Buffered_log *log= new (&m_root) Buffered_log(level, msg);
  if (log)
    m_list.push_back(log, &m_root);
}

/**
  Print buffered log messages.
*/
void Buffered_logs::print()
{
  Buffered_log *log;
  List_iterator_fast<Buffered_log> it(m_list);
  while ((log= it++))
    log->print();
}

/** Logs reported before a logger is available. */
static Buffered_logs buffered_logs;

struct my_rnd_struct sql_rand; ///< used by sql_class.cc:THD::THD()

#ifndef EMBEDDED_LIBRARY

Dynamic_array<MYSQL_SOCKET> listen_sockets(PSI_INSTRUMENT_MEM, 0);
bool unix_sock_is_online= false;
static int systemd_sock_activation; /* systemd socket activation */


C_MODE_START
#ifdef WITH_PERFSCHEMA_STORAGE_ENGINE
/**
  Error reporter that buffer log messages.
  @param level          log message level
  @param format         log message format string
*/
static void buffered_option_error_reporter(enum loglevel level,
                                           const char *format, ...)
{
  va_list args;
  char buffer[1024];

  va_start(args, format);
  my_vsnprintf(buffer, sizeof(buffer), format, args);
  va_end(args);
  buffered_logs.buffer(level, buffer);
}
#endif


/**
  Character set and collation error reporter that prints to sql error log.
  @param level          log message level
  @param format         log message format string

  This routine is used to print character set and collation
  warnings and errors inside an already running mysqld server,
  e.g. when a character set or collation is requested for the very first time
  and its initialization does not go well for some reasons.

  Note: At early mysqld initialization stage,
  when error log is not yet available,
  we use buffered_option_error_reporter() instead,
  to print general character set subsystem initialization errors,
  such as Index.xml syntax problems, bad XML tag hierarchy, etc.
*/
static void charset_error_reporter(enum loglevel level,
                                   const char *format, ...)
{
  va_list args;
  va_start(args, format);
  vprint_msg_to_log(level, format, args);
  va_end(args);                      
}
C_MODE_END

struct passwd *user_info;
static pthread_t select_thread;
#endif

/* OS specific variables */

#ifdef _WIN32
HANDLE hEventShutdown;
#endif


#ifndef EMBEDDED_LIBRARY
bool mysqld_embedded=0;
#else
bool mysqld_embedded=1;
#endif

my_bool plugins_are_initialized= FALSE;

#ifndef DBUG_OFF
static const char* default_dbug_option;
#endif
#ifdef HAVE_LIBWRAP
const char *libwrapName= NULL;
int allow_severity = LOG_INFO;
int deny_severity = LOG_WARNING;
#endif
#ifdef HAVE_QUERY_CACHE
ulong query_cache_min_res_unit= QUERY_CACHE_MIN_RESULT_DATA_SIZE;
Query_cache query_cache;
#endif


my_bool opt_use_ssl  = 0;
char *opt_ssl_ca= NULL, *opt_ssl_capath= NULL, *opt_ssl_cert= NULL,
  *opt_ssl_cipher= NULL, *opt_ssl_key= NULL, *opt_ssl_crl= NULL,
  *opt_ssl_crlpath= NULL, *opt_tls_version= NULL;
ulonglong tls_version= 0;

static scheduler_functions thread_scheduler_struct, extra_thread_scheduler_struct;
scheduler_functions *thread_scheduler= &thread_scheduler_struct,
                    *extra_thread_scheduler= &extra_thread_scheduler_struct;

#ifdef HAVE_OPENSSL
#include <openssl/crypto.h>
#if defined(HAVE_OPENSSL10) && !defined(HAVE_WOLFSSL)
typedef struct CRYPTO_dynlock_value
{
  mysql_rwlock_t lock;
} openssl_lock_t;

static openssl_lock_t *openssl_stdlocks;
static openssl_lock_t *openssl_dynlock_create(const char *, int);
static void openssl_dynlock_destroy(openssl_lock_t *, const char *, int);
static void openssl_lock_function(int, int, const char *, int);
static void openssl_lock(int, openssl_lock_t *, const char *, int);
#endif /* HAVE_OPENSSL10 */
char *des_key_file;
#ifndef EMBEDDED_LIBRARY
struct st_VioSSLFd *ssl_acceptor_fd;
#endif
#endif /* HAVE_OPENSSL */

/**
  Number of currently active user connections.
*/
static Atomic_counter<uint> connection_count;
static Atomic_counter<uint> extra_connection_count;

my_bool opt_gtid_strict_mode= FALSE;


/* Function declarations */

pthread_handler_t signal_hand(void *arg);
static int mysql_init_variables(void);
static int get_options(int *argc_ptr, char ***argv_ptr);
static bool add_terminator(DYNAMIC_ARRAY *options);
static bool add_many_options(DYNAMIC_ARRAY *, my_option *, size_t);
extern "C" my_bool mysqld_get_one_option(const struct my_option *, const char *,
                                         const char *);
static int init_thread_environment();
static char *get_relative_path(const char *path);
static int fix_paths(void);
#ifndef _WIN32
void handle_connections_sockets();
#endif

static bool read_init_file(char *file_name);
pthread_handler_t handle_slave(void *arg);
static void clean_up(bool print_message);
static int test_if_case_insensitive(const char *dir_name);

#ifndef EMBEDDED_LIBRARY
static bool pid_file_created= false;
static void usage(void);
static void start_signal_handler(void);
static void clean_up_mutexes(void);
static void wait_for_signal_thread_to_end(void);
static void create_pid_file();
ATTRIBUTE_NORETURN static void mysqld_exit(int exit_code);
#endif
static void delete_pid_file(myf flags);
static void end_ssl();


#ifndef EMBEDDED_LIBRARY
extern Atomic_counter<uint32_t> local_connection_thread_count;

uint THD_count::connection_thd_count()
{
  return value() -
    binlog_dump_thread_count -
    local_connection_thread_count;
}


/****************************************************************************
** Code to end mysqld
****************************************************************************/

/* common callee of two shutdown phases */
static void kill_thread(THD *thd)
{
  mysql_mutex_lock(&thd->LOCK_thd_kill);
  thd->abort_current_cond_wait(true);
  mysql_mutex_unlock(&thd->LOCK_thd_kill);
}


/**
  First shutdown everything but slave threads and binlog dump connections
*/
static my_bool kill_thread_phase_1(THD *thd, int *n_threads_awaiting_ack)
{
  DBUG_PRINT("quit", ("Informing thread %ld that it's time to die",
                      (ulong) thd->thread_id));

  if (thd->slave_thread || thd->is_binlog_dump_thread() ||
      (shutdown_wait_for_slaves &&
       repl_semisync_master.is_thd_awaiting_semisync_ack(thd) &&
       ++(*n_threads_awaiting_ack)))
    return 0;

  if (DBUG_IF("only_kill_system_threads") ? !thd->system_thread : 0)
    return 0;
  thd->awake(KILL_SERVER_HARD);
  return 0;
}


/**
  Last shutdown binlog dump connections
*/
static my_bool kill_thread_phase_2(THD *thd, void *)
{
  if (shutdown_wait_for_slaves && thd->is_binlog_dump_thread())
  {
    thd->set_killed(KILL_SERVER);
  }
  else
  {
    thd->set_killed(KILL_SERVER_HARD);
    MYSQL_CALLBACK(thread_scheduler, post_kill_notification, (thd));
  }
  kill_thread(thd);
  return 0;
}


/* associated with the kill thread phase 1 */
static my_bool warn_threads_active_after_phase_1(THD *thd, void *)
{
  if (!thd->is_binlog_dump_thread() && thd->vio_ok())
    sql_print_warning("%s: Thread %llu (user : '%s') did not exit\n", my_progname,
                      (ulonglong) thd->thread_id,
                      (thd->main_security_ctx.user ?
                       thd->main_security_ctx.user : ""));
  return 0;
}


/* associated with the kill thread phase 2 */
static my_bool warn_threads_active_after_phase_2(THD *thd, void *)
{
  mysql_mutex_lock(&thd->LOCK_thd_data);
  // dump thread may not have yet (or already) current_linfo set
  sql_print_warning("Dump thread %llu last sent to server %lu "
                    "binlog file:pos %s:%llu",
                    thd->thread_id, thd->variables.server_id,
                    thd->current_linfo ?
                    my_basename(thd->current_linfo->log_file_name) : "NULL",
                    thd->current_linfo ? thd->current_linfo->pos : 0);
  mysql_mutex_unlock(&thd->LOCK_thd_data);

  return 0;
}


/**
  Kills main thread.

  @note this function is responsible for setting abort_loop and breaking
  poll() in main thread. Shutdown as such is supposed to be performed by main
  thread itself.
*/

static void break_connect_loop()
{
#ifdef EXTRA_DEBUG
  int count=0;
#endif

  abort_loop= 1;

#if defined(_WIN32)
  mysqld_win_initiate_shutdown();
#else
  /* Avoid waiting for ourselves when thread-handling=no-threads. */
  if (pthread_equal(pthread_self(), select_thread))
    return;
  DBUG_PRINT("quit", ("waiting for select thread: %lu",
                      (ulong)select_thread));

  mysql_mutex_lock(&LOCK_start_thread);
  while (select_thread_in_use)
  {
    struct timespec abstime;
    int UNINIT_VAR(error);
    DBUG_PRINT("info",("Waiting for select thread"));

#ifndef DONT_USE_THR_ALARM
    if (pthread_kill(select_thread, thr_client_alarm))
      break;					// allready dead
#endif
    set_timespec(abstime, 2);
    for (uint tmp=0 ; tmp < 10 && select_thread_in_use; tmp++)
    {
      error= mysql_cond_timedwait(&COND_start_thread, &LOCK_start_thread,
                                  &abstime);
      if (error != EINTR)
        break;
    }
#ifdef EXTRA_DEBUG
    if (error != 0 && error != ETIMEDOUT && !count++)
      sql_print_error("Got error %d from mysql_cond_timedwait", error);
#endif
  }
  mysql_mutex_unlock(&LOCK_start_thread);
#endif /* _WIN32 */
}


/**
  A wrapper around kill_main_thrad().

  Sets shutdown user. This function may be called by multiple threads
  concurrently, thus it performs safe update of shutdown_user
  (first thread wins).
*/

void kill_mysql(THD *thd)
{
  char user_host_buff[MAX_USER_HOST_SIZE + 1];
  char *user, *expected_shutdown_user= 0;

  make_user_name(thd, user_host_buff);

  if ((user= my_strdup(PSI_NOT_INSTRUMENTED, user_host_buff, MYF(0))) &&
      !shutdown_user.compare_exchange_strong(expected_shutdown_user,
                                             user,
                                             std::memory_order_relaxed,
                                             std::memory_order_relaxed))
  {
    my_free(user);
  }

  shutdown_thread_id= thd->thread_id;
  DBUG_EXECUTE_IF("mysql_admin_shutdown_wait_for_slaves",
                  thd->lex->is_shutdown_wait_for_slaves= true;);
  DBUG_EXECUTE_IF("simulate_delay_at_shutdown",
                  {
                    DBUG_ASSERT(binlog_dump_thread_count == 3);
                    const char act[]=
                      "now "
                      "SIGNAL greetings_from_kill_mysql";
                    DBUG_ASSERT(!debug_sync_set_action(thd,
                                                       STRING_WITH_LEN(act)));
                  };);

  if (thd->lex->is_shutdown_wait_for_slaves)
    shutdown_wait_for_slaves= true;
  break_connect_loop();
}


static void close_connections(void)
{
  DBUG_ENTER("close_connections");

  /* Clear thread cache */
  thread_cache.final_flush();

  /* Abort listening to new connections */
  DBUG_PRINT("quit",("Closing sockets"));
  /* Protect against pthread_kill() calling close_server_sock(*) */
  mysql_mutex_lock(&LOCK_start_thread);
  for (uint i= 0 ; i < listen_sockets.elements() ; i++)
  {
    MYSQL_SOCKET *sock= listen_sockets.get_pos(i);
    (void) mysql_socket_close(*sock);
    if (sock->is_unix_domain_socket && !systemd_sock_activation)
    {
      (void) unlink(mysqld_unix_port);
    }
  }
  listen_sockets.free_memory();
  mysql_mutex_unlock(&LOCK_start_thread);

  end_thr_alarm(0);			 // Abort old alarms.

  while (CONNECT::count)
    my_sleep(100);

  /*
    First signal all threads that it's time to die
    This will give the threads some time to gracefully abort their
    statements and inform their clients that the server is about to die.
  */
  DBUG_EXECUTE_IF("mysqld_delay_kill_threads_phase_1", my_sleep(200000););
  int n_threads_awaiting_ack= 0;
  server_threads.iterate(kill_thread_phase_1, &n_threads_awaiting_ack);

  /*
    If we are waiting on any ACKs, delay killing the thread until either an ACK
    is received or the timeout is hit.

    Allow at max the number of sessions to await a timeout; however, if all
    ACKs have been received in less iterations, then quit early
  */
  if (shutdown_wait_for_slaves && repl_semisync_master.get_master_enabled())
  {
    int waiting_threads= repl_semisync_master.sync_get_master_wait_sessions();
    if (waiting_threads)
      sql_print_information("Delaying shutdown to await semi-sync ACK");

    while (waiting_threads-- > 0)
      repl_semisync_master.await_slave_reply();
  }

  DBUG_EXECUTE_IF("delay_shutdown_phase_2_after_semisync_wait",
                  my_sleep(500000););

  Events::deinit();
  slave_prepare_for_shutdown();
  mysql_bin_log.stop_background_thread();
  ack_receiver.stop();

  /*
    Give threads time to die.

    In 5.5, this was waiting 100 rounds @ 20 milliseconds/round, so as little
    as 2 seconds, depending on thread scheduling.

    From 10.0, we increase this to 1000 rounds / 20 seconds. The rationale is
    that on a server with heavy I/O load, it is quite possible for eg. an
    fsync() of the binlog or whatever to cause something like LOCK_log to be
    held for more than 2 seconds. We do not want to force kill threads in
    such cases, if it can be avoided. Note that normally, the wait will be
    much smaller than even 2 seconds, this is only a safety fallback against
    stuck threads so server shutdown is not held up forever.
  */
  DBUG_PRINT("info", ("THD_count: %u", THD_count::value()));

  for (int i= 0; THD_count::connection_thd_count() - n_threads_awaiting_ack
                 && i < 1000; i++)
    my_sleep(20000);

  if (global_system_variables.log_warnings)
    server_threads.iterate(warn_threads_active_after_phase_1);

#ifdef WITH_WSREP
  if (wsrep_inited == 1)
  {
    wsrep_deinit(true);
  }
  wsrep_sst_auth_free();
#endif
  /* All threads has now been aborted */
  DBUG_PRINT("quit", ("Waiting for threads to die (count=%u)",
                  THD_count::connection_thd_count() - n_threads_awaiting_ack));

  while (THD_count::connection_thd_count() - n_threads_awaiting_ack)
    my_sleep(1000);

  /* Kill phase 2 */
  server_threads.iterate(kill_thread_phase_2);
  for (uint64 i= 0; THD_count::value() > local_connection_thread_count; i++)
  {
    /*
      This time the warnings are emitted within the loop to provide a
      dynamic view on the shutdown status through the errorlog.
    */
    if (global_system_variables.log_warnings > 2 && i % 60000 == 0)
      server_threads.iterate(warn_threads_active_after_phase_2);
    my_sleep(1000);
  }
  /* End of kill phase 2 */

  DBUG_PRINT("quit",("close_connections thread"));
  DBUG_VOID_RETURN;
}

#endif /*EMBEDDED_LIBRARY*/


extern "C" sig_handler print_signal_warning(int sig)
{
  if (global_system_variables.log_warnings)
    sql_print_warning("Got signal %d from thread %u", sig,
                      (uint)my_thread_id());
#ifdef SIGNAL_HANDLER_RESET_ON_DELIVERY
  my_sigset(sig,print_signal_warning);		/* int. thread system calls */
#endif
#if !defined(_WIN32)
  if (sig == SIGALRM)
    alarm(2);					/* reschedule alarm */
#endif
}

#ifndef EMBEDDED_LIBRARY
extern "C" void unireg_abort(int exit_code)
{
  DBUG_ENTER("unireg_abort");

  if (opt_help)
    usage();
  else if (exit_code)
    sql_print_error("Aborting");
  /* Don't write more notes to the log to not hide error message */
  disable_log_notes= 1;

#ifdef WITH_WSREP
  // Note that we do not have thd here, thus can't use
  // WSREP(thd)

  if (WSREP_ON &&
      Wsrep_server_state::is_inited() &&
      Wsrep_server_state::instance().state() != wsrep::server_state::s_disconnected)
  {
    /*
      This is an abort situation, we cannot expect to gracefully close all
      wsrep threads here, we can only diconnect from service
    */
    wsrep_close_client_connections(FALSE);
    Wsrep_server_state::instance().disconnect();
    WSREP_INFO("Service disconnected.");
    wsrep_close_threads(NULL); /* this won't close all threads */
    sleep(1); /* so give some time to exit for those which can */
    WSREP_INFO("Some threads may fail to exit.");
  }

  if (WSREP_ON && wsrep_inited)
  {
    wsrep_deinit(true);
    wsrep_deinit_server();
  }
  wsrep_sst_auth_free();
#endif // WITH_WSREP

  clean_up(!opt_abort && (exit_code || !opt_bootstrap)); /* purecov: inspected */
  DBUG_PRINT("quit",("done with cleanup in unireg_abort"));
  mysqld_exit(exit_code);
}

#ifdef _WIN32
typedef void (*report_svc_status_t)(DWORD current_state, DWORD win32_exit_code,
                                    DWORD wait_hint);
static void dummy_svc_status(DWORD, DWORD, DWORD) {}
static report_svc_status_t my_report_svc_status= dummy_svc_status;
#endif

static void mysqld_exit(int exit_code)
{
  DBUG_ENTER("mysqld_exit");
  /*
    Important note: we wait for the signal thread to end,
    but if a kill -15 signal was sent, the signal thread did
    spawn the kill_server_thread thread, which is running concurrently.
  */
  rpl_deinit_gtid_waiting();
  rpl_deinit_gtid_slave_state();
  wait_for_signal_thread_to_end();
#ifdef WITH_WSREP
  wsrep_deinit_server();
  wsrep_sst_auth_free();
#endif /* WITH_WSREP */
  mysql_audit_finalize();
  clean_up_mutexes();
  my_end((opt_endinfo ? MY_CHECK_ERROR | MY_GIVE_INFO : 0));
#ifdef WITH_PERFSCHEMA_STORAGE_ENGINE
  shutdown_performance_schema();        // we do it as late as possible
#endif
  set_malloc_size_cb(NULL);
  if (global_status_var.global_memory_used)
  {
    fprintf(stderr, "Warning: Memory not freed: %lld\n",
            (longlong) global_status_var.global_memory_used);
    if (exit_code == 0 || opt_endinfo)
      SAFEMALLOC_REPORT_MEMORY(0);
  }
  DBUG_LEAVE;
#ifdef _WIN32
  my_report_svc_status(SERVICE_STOPPED, exit_code, 0);
#endif
  sd_notify(0, "STATUS=MariaDB server is down");
  exit(exit_code); /* purecov: inspected */
}

#endif /* !EMBEDDED_LIBRARY */

static void clean_up(bool print_message)
{
  DBUG_PRINT("exit",("clean_up"));
  if (cleanup_done++)
    return; /* purecov: inspected */

#ifdef HAVE_REPLICATION
  // We must call end_slave() as clean_up may have been called during startup
  end_slave();
  if (use_slave_mask)
    my_bitmap_free(&slave_error_mask);
#endif
  stop_handle_manager();
  ddl_log_release();

  logger.cleanup_base();

  injector::free_instance();
  mysql_bin_log.cleanup();

  my_tz_free();
  my_dboptions_cache_free();
  ignore_db_dirs_free();
  servers_free(1);
#ifndef NO_EMBEDDED_ACCESS_CHECKS
  acl_free(1);
  grant_free();
#endif
  query_cache_destroy();
  hostname_cache_free();
  item_func_sleep_free();
  lex_free();				/* Free some memory */
  item_create_cleanup();
  tdc_start_shutdown();
#ifdef HAVE_REPLICATION
  semi_sync_master_deinit();
#endif
  plugin_shutdown();
  udf_free();
  ha_end();
  if (tc_log)
    tc_log->close();
  xid_cache_free();
  tdc_deinit();
  mdl_destroy();
  dflt_key_cache= 0;
  key_caches.delete_elements(free_key_cache);
  wt_end();
  multi_keycache_free();
  sp_cache_end();
  free_status_vars();
  end_thr_alarm(1);			/* Free allocated memory */
  end_thr_timer();
  my_free_open_file_info();
  if (defaults_argv)
    free_defaults(defaults_argv);
  free_tmpdir(&mysql_tmpdir_list);
  my_bitmap_free(&temp_pool);
  free_max_user_conn();
  free_global_user_stats();
  free_global_client_stats();
  free_global_table_stats();
  free_global_index_stats();
  delete_dynamic(&all_options);                 // This should be empty
  free_all_rpl_filters();
  wsrep_thr_deinit();
  my_uuid_end();
  delete type_handler_data;
  delete binlog_filter;
  delete global_rpl_filter;
  end_ssl();
#ifndef EMBEDDED_LIBRARY
  vio_end();
#endif /*!EMBEDDED_LIBRARY*/
#if defined(ENABLED_DEBUG_SYNC)
  /* End the debug sync facility. See debug_sync.cc. */
  debug_sync_end();
#endif /* defined(ENABLED_DEBUG_SYNC) */

  delete_pid_file(MYF(0));

  if (print_message && my_default_lc_messages && server_start_time)
    sql_print_information(ER_DEFAULT(ER_SHUTDOWN_COMPLETE),my_progname);
  MYSQL_CALLBACK(thread_scheduler, end, ());
  thread_scheduler= 0;
  mysql_library_end();
  finish_client_errs();
  free_root(&startup_root, MYF(0));
  protect_root(&read_only_root, PROT_READ | PROT_WRITE);
  free_root(&read_only_root, MYF(0));
  cleanup_errmsgs();
  free_error_messages();
  /* Tell main we are ready */
  logger.cleanup_end();
  sys_var_end();
  free_charsets();

  my_free(const_cast<char*>(log_bin_basename));
  my_free(const_cast<char*>(log_bin_index));
#ifndef EMBEDDED_LIBRARY
  my_free(const_cast<char*>(relay_log_basename));
  my_free(const_cast<char*>(relay_log_index));
#endif
  free_list(opt_plugin_load_list_ptr);
  destroy_proxy_protocol_networks();

  /*
    The following lines may never be executed as the main thread may have
    killed us
  */
  DBUG_PRINT("quit", ("done with cleanup"));
} /* clean_up */


#ifndef EMBEDDED_LIBRARY

/**
  This is mainly needed when running with purify, but it's still nice to
  know that all child threads have died when mysqld exits.
*/
static void wait_for_signal_thread_to_end()
{
  uint i;
  /*
    Wait up to 10 seconds for signal thread to die. We use this mainly to
    avoid getting warnings that my_thread_end has not been called
  */
  for (i= 0 ; i < 100 && signal_thread_in_use; i++)
  {
    if (pthread_kill(signal_thread, MYSQL_KILL_SIGNAL) == ESRCH)
      break;
    my_sleep(100);				// Give it time to die
  }
}
#endif /*EMBEDDED_LIBRARY*/

static void clean_up_mutexes()
{
  DBUG_ENTER("clean_up_mutexes");
  server_threads.destroy();
  thread_cache.destroy();
  mysql_rwlock_destroy(&LOCK_grant);
  mysql_mutex_destroy(&LOCK_start_thread);
  mysql_mutex_destroy(&LOCK_status);
  mysql_rwlock_destroy(&LOCK_all_status_vars);
  mysql_mutex_destroy(&LOCK_delayed_insert);
  mysql_mutex_destroy(&LOCK_delayed_status);
  mysql_mutex_destroy(&LOCK_delayed_create);
  mysql_mutex_destroy(&LOCK_crypt);
  mysql_mutex_destroy(&LOCK_user_conn);
  mysql_mutex_destroy(&LOCK_thread_id);
  mysql_mutex_destroy(&LOCK_stats);
  mysql_mutex_destroy(&LOCK_global_user_client_stats);
  mysql_mutex_destroy(&LOCK_global_table_stats);
  mysql_mutex_destroy(&LOCK_global_index_stats);
#ifdef HAVE_OPENSSL
  mysql_mutex_destroy(&LOCK_des_key_file);
#if defined(HAVE_OPENSSL10) && !defined(HAVE_WOLFSSL)
  for (int i= 0; i < CRYPTO_num_locks(); ++i)
    mysql_rwlock_destroy(&openssl_stdlocks[i].lock);
  OPENSSL_free(openssl_stdlocks);
#endif /* HAVE_OPENSSL10 */
#endif /* HAVE_OPENSSL */
#ifdef HAVE_REPLICATION
  mysql_mutex_destroy(&LOCK_rpl_status);
#endif /* HAVE_REPLICATION */
  mysql_mutex_destroy(&LOCK_active_mi);
  mysql_rwlock_destroy(&LOCK_ssl_refresh);
  mysql_mutex_destroy(&LOCK_backup_log);
  mysql_mutex_destroy(&LOCK_temp_pool);
  mysql_rwlock_destroy(&LOCK_sys_init_connect);
  mysql_rwlock_destroy(&LOCK_sys_init_slave);
  mysql_mutex_destroy(&LOCK_global_system_variables);
  mysql_prlock_destroy(&LOCK_system_variables_hash);
  mysql_mutex_destroy(&LOCK_short_uuid_generator);
  mysql_mutex_destroy(&LOCK_prepared_stmt_count);
  mysql_mutex_destroy(&LOCK_error_messages);
  mysql_cond_destroy(&COND_start_thread);
  mysql_mutex_destroy(&LOCK_server_started);
  mysql_cond_destroy(&COND_server_started);
  mysql_mutex_destroy(&LOCK_prepare_ordered);
  mysql_cond_destroy(&COND_prepare_ordered);
  mysql_mutex_destroy(&LOCK_after_binlog_sync);
  mysql_mutex_destroy(&LOCK_commit_ordered);
#ifndef EMBEDDED_LIBRARY
  mysql_mutex_destroy(&LOCK_error_log);
#endif
  DBUG_VOID_RETURN;
}


/****************************************************************************
** Init IP and UNIX socket
****************************************************************************/

#ifdef EMBEDDED_LIBRARY
void close_connection(THD *thd, uint sql_errno)
{
}
#else
static void set_ports()
{
  char	*env;
  if (!mysqld_port && !opt_disable_networking)
  {					// Get port if not from commandline
    mysqld_port= MYSQL_PORT;

    /*
      if builder specifically requested a default port, use that
      (even if it coincides with our factory default).
      only if they didn't do we check /etc/services (and, failing
      on that, fall back to the factory default of 3306).
      either default can be overridden by the environment variable
      MYSQL_TCP_PORT, which in turn can be overridden with command
      line options.
    */

#if MYSQL_PORT_DEFAULT == 0
# if !__has_feature(memory_sanitizer) // Work around MSAN deficiency
    struct  servent *serv_ptr;
    if ((serv_ptr= getservbyname("mysql", "tcp")))
      SYSVAR_AUTOSIZE(mysqld_port, ntohs((u_short) serv_ptr->s_port));
# endif
#endif
    if ((env = getenv("MYSQL_TCP_PORT")))
    {
      mysqld_port= (uint) atoi(env);
      set_sys_var_value_origin(&mysqld_port, sys_var::ENV);
    }
  }
  if (!mysqld_unix_port)
  {
#ifdef _WIN32
    mysqld_unix_port= (char*) MYSQL_NAMEDPIPE;
#else
    mysqld_unix_port= (char*) MYSQL_UNIX_ADDR;
#endif
    if ((env = getenv("MYSQL_UNIX_PORT")))
    {
      mysqld_unix_port= env;
      set_sys_var_value_origin(&mysqld_unix_port, sys_var::ENV);
    }
  }
}

/* Change to run as another user if started with --user */

static struct passwd *check_user(const char *user)
{
  myf flags= 0;
  if (global_system_variables.log_warnings)
    flags|= MY_WME;
  if (!opt_bootstrap && !opt_help)
    flags|= MY_FAE;

  struct passwd *tmp_user_info= my_check_user(user, MYF(flags));

  if (!tmp_user_info && my_errno==EINVAL && (flags & MY_FAE))
    unireg_abort(1);

  return tmp_user_info;
}

static inline void allow_coredumps()
{
#ifdef PR_SET_DUMPABLE
  if (test_flags & TEST_CORE_ON_SIGNAL)
  {
    /* inform kernel that process is dumpable */
    (void) prctl(PR_SET_DUMPABLE, 1);
  }
#endif
}


static void set_user(const char *user, struct passwd *user_info_arg)
{
  /*
    We can get a SIGSEGV when calling initgroups() on some systems when NSS
    is configured to use LDAP and the server is statically linked.  We set
    calling_initgroups as a flag to the SIGSEGV handler that is then used to
    output a specific message to help the user resolve this problem.
  */
  calling_initgroups= 1;
  int res= my_set_user(user, user_info_arg, MYF(MY_WME));
  calling_initgroups= 0;
  if (res)
    unireg_abort(1);
  allow_coredumps();
}

#if !defined(_WIN32)
static void set_effective_user(struct passwd *user_info_arg)
{
  DBUG_ASSERT(user_info_arg != 0);
  if (setregid((gid_t)-1, user_info_arg->pw_gid) == -1)
  {
    sql_perror("setregid");
    unireg_abort(1);
  }
  if (setreuid((uid_t)-1, user_info_arg->pw_uid) == -1)
  {
    sql_perror("setreuid");
    unireg_abort(1);
  }
  allow_coredumps();
}
#endif

/** Change root user if started with @c --chroot . */
static void set_root(const char *path)
{
#if !defined(_WIN32)
  if (chroot(path) == -1)
  {
    sql_perror("chroot");
    unireg_abort(1);
  }
  my_setwd("/", MYF(0));
#endif
}

/**
   Activate usage of a tcp port
*/

static void activate_tcp_port(uint port,
                              Dynamic_array<MYSQL_SOCKET> *sockets,
                              bool is_extra_port= false)
{
  struct addrinfo *ai, *a;
  struct addrinfo hints;
  int error;
  int	arg;
  char port_buf[NI_MAXSERV];
  const char *real_bind_addr_str;
  MYSQL_SOCKET ip_sock= MYSQL_INVALID_SOCKET;
  DBUG_ENTER("activate_tcp_port");
  DBUG_PRINT("general",("IP Socket is %d",port));

  bzero(&hints, sizeof (hints));
  hints.ai_flags= AI_PASSIVE;
  hints.ai_socktype= SOCK_STREAM;
  hints.ai_family= AF_UNSPEC;
  
  if (my_bind_addr_str && strcmp(my_bind_addr_str, "*") == 0)
    real_bind_addr_str= NULL; // windows doesn't seem to support * here
  else
    real_bind_addr_str= my_bind_addr_str;

  my_snprintf(port_buf, NI_MAXSERV, "%d", port);
  error= getaddrinfo(real_bind_addr_str, port_buf, &hints, &ai);
  if (unlikely(error != 0))
  {
    DBUG_PRINT("error",("Got error: %d from getaddrinfo()", error));

    sql_print_error("%s: %s", ER_DEFAULT(ER_IPSOCK_ERROR), gai_strerror(error));
    unireg_abort(1);				/* purecov: tested */
  }

  for (a= ai; a != NULL; a= a->ai_next)
  {
    ip_sock= mysql_socket_socket(key_socket_tcpip, a->ai_family,
                                 a->ai_socktype, a->ai_protocol);

    char ip_addr[INET6_ADDRSTRLEN];
    if (vio_get_normalized_ip_string(a->ai_addr, a->ai_addrlen,
                                     ip_addr, sizeof (ip_addr)))
    {
      ip_addr[0]= 0;
    }

    if (mysql_socket_getfd(ip_sock) == INVALID_SOCKET)
    {
      sql_print_message_func func= real_bind_addr_str ? sql_print_error
                                                      : sql_print_warning;
      func("Failed to create a socket for %s '%s': errno: %d.",
           (a->ai_family == AF_INET) ? "IPv4" : "IPv6",
           (const char *) ip_addr, (int) socket_errno);
    }
    else 
    {
      ip_sock.address_family= a->ai_family;
      sql_print_information("Server socket created on IP: '%s'.",
                          (const char *) ip_addr);

      if (mysql_socket_getfd(ip_sock) == INVALID_SOCKET)
      {
        DBUG_PRINT("error",("Got error: %d from socket()",socket_errno));
        sql_perror(ER_DEFAULT(ER_IPSOCK_ERROR));  /* purecov: tested */
        unireg_abort(1);                          /* purecov: tested */
      }

      mysql_socket_set_thread_owner(ip_sock);

#ifndef _WIN32
      /*
        We should not use SO_REUSEADDR on windows as this would enable a
        user to open two mysqld servers with the same TCP/IP port.
      */
      arg= 1;
      (void) mysql_socket_setsockopt(ip_sock, SOL_SOCKET, SO_REUSEADDR,
                                     (char*)&arg, sizeof(arg));
#endif /* _WIN32 */

#ifdef IPV6_V6ONLY
      /*
        If an address name resolves to both IPv4 and IPv6 addresses, the server
        will listen on them both. With IPV6_V6ONLY unset, listening on an IPv6
        wildcard address may cause listening on an IPv4 wildcard address
        to fail. That's why IPV6_V6ONLY needs to be forcefully turned on.
      */
      if (a->ai_family == AF_INET6)
      {
        arg= 1;
        (void) mysql_socket_setsockopt(ip_sock, IPPROTO_IPV6, IPV6_V6ONLY,
                                       (char*)&arg, sizeof(arg));
      }
#endif

#ifdef IP_FREEBIND
      arg= 1;
      (void) mysql_socket_setsockopt(ip_sock, IPPROTO_IP, IP_FREEBIND,
                                     (char*) &arg, sizeof(arg));
#endif
      /*
        Sometimes the port is not released fast enough when stopping and
        restarting the server. This happens quite often with the test suite
        on busy Linux systems. Retry to bind the address at these intervals:
        Sleep intervals: 1, 2, 4,  6,  9, 13, 17, 22, ...
        Retry at second: 1, 3, 7, 13, 22, 35, 52, 74, ...
        Limit the sequence by mysqld_port_timeout (set --port-open-timeout=#).
      */
      int ret;
      uint waited, retry, this_wait;
      for (waited= 0, retry= 1; ; retry++, waited+= this_wait)
      {
        if (((ret= mysql_socket_bind(ip_sock, a->ai_addr, a->ai_addrlen)) >= 0 )
            || (socket_errno != SOCKET_EADDRINUSE)
            || (waited >= mysqld_port_timeout))
          break;
        sql_print_information("Retrying bind on TCP/IP port %u", port);
        this_wait= retry * retry / 3 + 1;
        sleep(this_wait);
      }

      if (ret < 0)
      {
        char buff[100];
        sprintf(buff, "Can't start server: Bind on TCP/IP port. Got error: %d",
                (int) socket_errno);
        sql_perror(buff);
        sql_print_error("Do you already have another server running on "
                        "port: %u ?", port);
        unireg_abort(1);
      }
      if (mysql_socket_listen(ip_sock,(int) back_log) < 0)
      {
        sql_perror("Can't start server: listen() on TCP/IP port");
        sql_print_error("listen() on TCP/IP failed with error %d",
                        socket_errno);
        unireg_abort(1);
      }

#ifdef FD_CLOEXEC
      (void) fcntl(mysql_socket_getfd(ip_sock), F_SETFD, FD_CLOEXEC);
#endif
      ip_sock.is_extra_port= is_extra_port;
      sockets->push(ip_sock);
    }
  }

  freeaddrinfo(ai);
  DBUG_VOID_RETURN;
}


/**
   Activate usage of a systemd activated sockets
   i.e started by mariadb.socket
*/

static void use_systemd_activated_sockets()
{
#ifndef __linux__
  return;
#else
  char **names = NULL;
  int sd_sockets;
  DBUG_ENTER("use_systemd_activated_sockets");

  sd_sockets= sd_listen_fds_with_names(0, &names);

  if (!sd_sockets)
    DBUG_VOID_RETURN;

  DBUG_PRINT("general",("Systemd listen_fds is %d", sd_sockets));
  while (sd_sockets--)
  {
    MYSQL_SOCKET sock;
    int stype= 0, accepting= 0, getnameinfo_err;
    socklen_t l;
    union
    {
          struct sockaddr sa;
          struct sockaddr_storage storage;
          struct sockaddr_in in;
          struct sockaddr_in6 in6;
          struct sockaddr_un un;
    } addr;
    SOCKET_SIZE_TYPE addrlen= sizeof(addr);
    char hbuf[NI_MAXHOST], sbuf[NI_MAXSERV];

    int fd= SD_LISTEN_FDS_START + sd_sockets;

    if (getsockname(fd, &addr.sa, &addrlen))
    {
      sql_print_error("Unable to getsockname on systemd socket activation socket %d,"
                      " errno %d", fd, errno);
      goto err;
    }

    l= sizeof(stype);
    if (getsockopt(fd, SOL_SOCKET, SO_TYPE, &stype, &l) < 0)
    {
      sql_print_error("Unable to getsockopt(SOL_SOCKET, SO_TYPE) on"
                      " systemd socket activation socket %d,"
                      " errno %d", fd, errno);
      goto err;
    }

    if (stype != SOCK_STREAM)
    {
      sql_print_error("Unknown systemd socket activation socket %d,"
                      " not of type SOCK_STREAM - type %d", fd, stype);
      goto err;
    }

    l= sizeof(accepting);
    if (getsockopt(fd, SOL_SOCKET, SO_ACCEPTCONN, &accepting, &l) < 0)
    {
      sql_print_error("Unable to getsockopt(SOL_SOCKET, SO_ACCEPTCONN) on"
                      " systemd socket activation socket %d,"
                      " errno %d", fd, errno);
      goto err;
    }

    if (!accepting)
    {
      sql_print_error("Unknown systemd socket activation socket %d,"
                      " is not listening", fd);
      goto err;
    }

    switch (addr.sa.sa_family)
    {
    case AF_INET:
      sock= mysql_socket_fd(key_socket_tcpip, fd);
      sock.is_unix_domain_socket= 0;
      mysqld_port= ntohs(addr.in.sin_port);
      break;
    case AF_INET6:
      sock= mysql_socket_fd(key_socket_tcpip, fd);
      sock.is_unix_domain_socket= 0;
      mysqld_port= ntohs(addr.in6.sin6_port);
      break;
    case AF_UNIX:
      sock= mysql_socket_fd(key_socket_unix, fd);
      sock.is_unix_domain_socket= 1;
      break;
    default:
      sql_print_error("Unknown systemd socket activation socket %d,"
                      " not UNIX or INET socket", fd);
      goto err;
    }

    /*
      We check names!=NULL here because sd_listen_fds_with_names maybe
      just sd_listen_fds on older pre v227 systemd
    */
    sock.is_extra_port= names && strcmp(names[sd_sockets], "extra") == 0;

    if (addr.sa.sa_family == AF_UNIX)
    {
      /*
        Handle abstract sockets and present them in @ form.
      */
      if (addr.un.sun_path[0] == '\0')
        addr.un.sun_path[0] = '@';
      sql_print_information("Using systemd activated unix socket %s%s",
                            addr.un.sun_path, sock.is_extra_port ? " (extra)" : "");
      memset(addr.un.sun_path, 0, sizeof(addr.un.sun_path));
    }
    else
    {
      getnameinfo_err= getnameinfo(&addr.sa, addrlen, hbuf, sizeof(hbuf), sbuf,
                                   sizeof(sbuf), NI_NUMERICHOST | NI_NUMERICSERV);
      if (getnameinfo_err)
        sql_print_warning("getnameinfo() on systemd socket activation socket %d"
                          " failed with error %s(%d)", fd,
                          gai_strerror(getnameinfo_err), getnameinfo_err);
      else
        sql_print_information("Using systemd activated socket host %s port %s%s", hbuf, sbuf,
                              sock.is_extra_port ? " (extra)" : "");
    }

    mysql_socket_set_thread_owner(sock);
    listen_sockets.push(sock);
  }
  systemd_sock_activation= 1;
  free(names);

  DBUG_VOID_RETURN;

err:
  free(names);
  unireg_abort(1);
  DBUG_VOID_RETURN;
#endif /* __linux__ */
}


static void network_init(void)
{
#ifdef HAVE_SYS_UN_H
  struct sockaddr_un	UNIXaddr;
  int	arg;
#endif
  DBUG_ENTER("network_init");

  use_systemd_activated_sockets();

  if (MYSQL_CALLBACK_ELSE(thread_scheduler, init, (), 0))
    unireg_abort(1);			/* purecov: inspected */

  if (init_proxy_protocol_networks(my_proxy_protocol_networks))
    unireg_abort(1);

  set_ports();

  if (report_port == 0)
  {
    SYSVAR_AUTOSIZE(report_port, mysqld_port);
  }
#ifndef DBUG_OFF
  if (!opt_disable_networking)
    DBUG_ASSERT(report_port != 0);
#endif
  if (!opt_disable_networking && !opt_bootstrap && !systemd_sock_activation)
  {
    if (mysqld_port)
      activate_tcp_port(mysqld_port, &listen_sockets,
                        /* is_extra_port= */ false);
    if (mysqld_extra_port)
      activate_tcp_port(mysqld_extra_port, &listen_sockets,
                        /* is_extra_port= */ true);
  }

#if defined(HAVE_SYS_UN_H)
  /*
  ** Create the UNIX socket
  */
  if (mysqld_unix_port[0] && !opt_bootstrap && systemd_sock_activation==0)
  {
    MYSQL_SOCKET unix_sock= MYSQL_INVALID_SOCKET;
    size_t port_len;
    DBUG_PRINT("general",("UNIX Socket is %s",mysqld_unix_port));

    if ((port_len= strlen(mysqld_unix_port)) > sizeof(UNIXaddr.sun_path) - 1)
    {
      sql_print_error("The socket file path is too long (> %u): %s",
                      (uint) sizeof(UNIXaddr.sun_path) - 1, mysqld_unix_port);
      unireg_abort(1);
    }
    unix_sock= mysql_socket_socket(key_socket_unix, AF_UNIX, SOCK_STREAM, 0);
    if (mysql_socket_getfd(unix_sock) < 0)
    {
      sql_perror("Can't start server : UNIX Socket "); /* purecov: inspected */
      unireg_abort(1);				/* purecov: inspected */
    }

    unix_sock.is_unix_domain_socket= true;
    listen_sockets.push(unix_sock);
    unix_sock_is_online= true;
    mysql_socket_set_thread_owner(unix_sock);

    bzero((char*) &UNIXaddr, sizeof(UNIXaddr));
    UNIXaddr.sun_family = AF_UNIX;
    strmov(UNIXaddr.sun_path, mysqld_unix_port);
#if defined(__linux__)
    /* Abstract socket */
    if (mysqld_unix_port[0] == '@')
    {
      UNIXaddr.sun_path[0]= '\0';
      port_len+= offsetof(struct sockaddr_un, sun_path);
    }
    else
#endif
    {
      (void) unlink(mysqld_unix_port);
      port_len= sizeof(UNIXaddr);
    }
    arg= 1;
    (void) mysql_socket_setsockopt(unix_sock,SOL_SOCKET,SO_REUSEADDR,
                                   (char*)&arg, sizeof(arg));
    umask(0);
    if (mysql_socket_bind(unix_sock,
                          reinterpret_cast<struct sockaddr *>(&UNIXaddr),
                          port_len) < 0)
    {
      sql_perror("Can't start server : Bind on unix socket"); /* purecov: tested */
      sql_print_error("Do you already have another server running on socket: %s ?",mysqld_unix_port);
      unireg_abort(1);					/* purecov: tested */
    }
    umask(((~my_umask) & 0666));
#if defined(S_IFSOCK) && defined(SECURE_SOCKETS)
    (void) chmod(mysqld_unix_port,S_IFSOCK);	/* Fix solaris 2.6 bug */
#endif
    if (mysql_socket_listen(unix_sock,(int) back_log) < 0)
      sql_print_warning("listen() on Unix socket failed with error %d",
		      socket_errno);
#ifdef FD_CLOEXEC
    (void) fcntl(mysql_socket_getfd(unix_sock), F_SETFD, FD_CLOEXEC);
#endif
  }
#endif

#ifdef _WIN32
  network_init_win();
#endif

  DBUG_PRINT("info",("server started"));
  DBUG_VOID_RETURN;
}


/**
  Close a connection.

  @param thd        Thread handle.
  @param sql_errno  The error code to send before disconnect.

  @note
    For the connection that is doing shutdown, this is called twice
*/

void close_connection(THD *thd, uint sql_errno)
{
  int lvl= (thd->main_security_ctx.user ? 3 : 1);
  DBUG_ENTER("close_connection");

  if (sql_errno)
  {
    thd->protocol->net_send_error(thd, sql_errno, ER_DEFAULT(sql_errno), NULL);
    thd->print_aborted_warning(lvl, ER_DEFAULT(sql_errno));
  }
  else
    thd->print_aborted_warning(lvl, (thd->main_security_ctx.user ?
                                     "This connection closed normally" :
                                     "This connection closed normally without"
                                      " authentication"));

  thd->disconnect();

  MYSQL_CONNECTION_DONE((int) sql_errno, thd->thread_id);

  if (MYSQL_CONNECTION_DONE_ENABLED())
  {
    sleep(0); /* Workaround to avoid tailcall optimisation */
  }
  mysql_audit_notify_connection_disconnect(thd, sql_errno);
  DBUG_VOID_RETURN;
}


/** Called when mysqld is aborted with ^C */
/* ARGSUSED */
extern "C" sig_handler end_mysqld_signal(int sig __attribute__((unused)))
{
  DBUG_ENTER("end_mysqld_signal");
  /* Don't kill if signal thread is not running */
  if (signal_thread_in_use)
    break_connect_loop();                         // Take down mysqld nicely
  DBUG_VOID_RETURN;				/* purecov: deadcode */
}
#endif /* EMBEDDED_LIBRARY */


/*
  Unlink thd from global list of available connections

  SYNOPSIS
    unlink_thd()
    thd		 Thread handler
*/

void unlink_thd(THD *thd)
{
  DBUG_ENTER("unlink_thd");
  DBUG_PRINT("enter", ("thd: %p", thd));

  thd->cleanup();
  thd->add_status_to_global();
  server_threads.erase(thd);

#ifdef WITH_WSREP
  /*
    Do not decrement when its wsrep system thread. wsrep_applier is set for
    applier as well as rollbacker threads.
  */
  if (!thd->wsrep_applier)
#endif /* WITH_WSREP */
  --*thd->scheduler->connection_count;

  thd->free_connection();

  DBUG_VOID_RETURN;
}


#if defined(_WIN32)
/*
  If server is started as service, the service routine will set
  the callback function.
*/
void mysqld_set_service_status_callback(void (*r)(DWORD, DWORD, DWORD))
{
  my_report_svc_status= r;
}

static bool startup_complete()
{
  return hEventShutdown != NULL;
}

/**
  Initiates shutdown on Windows by setting shutdown event.
  Reports windows service status.

  If startup was not finished, terminates process (no good
  cleanup possible)
*/
void mysqld_win_initiate_shutdown()
{
  if (startup_complete())
  {
    my_report_svc_status(SERVICE_STOP_PENDING, 0, 0);
    abort_loop= 1;
    if (!SetEvent(hEventShutdown))
      /* This should never fail.*/
      abort();
  }
  else
  {
    my_report_svc_status(SERVICE_STOPPED, 1, 0);
    TerminateProcess(GetCurrentProcess(), 1);
  }
}

/*
  Signal when server has started and can accept connections.
*/
void mysqld_win_set_startup_complete()
{
  my_report_svc_status(SERVICE_RUNNING, 0, 0);
  DBUG_ASSERT(startup_complete());
}


void mysqld_win_extend_service_timeout(DWORD sec)
{
  my_report_svc_status((DWORD)-1, 0, 2*1000*sec);
}


void mysqld_win_set_service_name(const char *name)
{
  if (stricmp(name, "mysql"))
    load_default_groups[array_elements(load_default_groups) - 2]= name;
}

/*
  On Windows, we use native SetConsoleCtrlHandler for handle events like Ctrl-C
  with graceful shutdown.
  Also, we do not use signal(), but SetUnhandledExceptionFilter instead - as it
  provides possibility to pass the exception to just-in-time debugger, collect
  dumps and potentially also the exception and thread context used to output
  callstack.
*/

static BOOL WINAPI console_event_handler( DWORD type )
{
  static const char *names[]= {
   "CTRL_C_EVENT","CTRL_BREAK_EVENT", "CTRL_CLOSE_EVENT", "", "",
   "CTRL_LOGOFF_EVENT", "CTRL_SHUTDOWN_EVENT"};

  switch (type)
  {
  case CTRL_C_EVENT:
  case CTRL_BREAK_EVENT:
    sql_print_information("console_event_handler: received %s event, shutting down",
                          names[type]);
    mysqld_win_initiate_shutdown();
    return TRUE;
  case CTRL_CLOSE_EVENT:
    sql_print_information("console_event_handler: received CTRL_CLOSE_EVENT event, terminating");
    TerminateProcess(GetCurrentProcess(), 1);
    return TRUE;
  default:
    return FALSE;
  }
}


#ifdef DEBUG_UNHANDLED_EXCEPTION_FILTER
#define DEBUGGER_ATTACH_TIMEOUT 120
/*
  Wait for debugger to attach and break into debugger. If debugger is
  not attached, resume after timeout.
*/
static void wait_for_debugger(int timeout_sec)
{
   if(!IsDebuggerPresent())
   {
     int i;
     printf("Waiting for debugger to attach, pid=%u\n",GetCurrentProcessId());
     fflush(stdout);
     for(i= 0; i < timeout_sec; i++)
     {
       Sleep(1000);
       if(IsDebuggerPresent())
       {
         /* Break into debugger */
         __debugbreak();
         return;
       }
     }
     printf("pid=%u, debugger not attached after %d seconds, resuming\n",GetCurrentProcessId(),
       timeout_sec);
     fflush(stdout);
   }
}
#endif /* DEBUG_UNHANDLED_EXCEPTION_FILTER */

static LONG WINAPI my_unhandler_exception_filter(EXCEPTION_POINTERS *ex_pointers)
{
   static BOOL first_time= TRUE;
   if(!first_time)
   {
     /*
       This routine can be called twice, typically
       when detaching in JIT debugger.
       Return EXCEPTION_EXECUTE_HANDLER to terminate process.
     */
     return EXCEPTION_EXECUTE_HANDLER;
   }
   first_time= FALSE;
#ifdef DEBUG_UNHANDLED_EXCEPTION_FILTER
   /*
    Unfortunately there is no clean way to debug unhandled exception filters,
    as debugger does not stop there(also documented in MSDN) 
    To overcome, one could put a MessageBox, but this will not work in service.
    Better solution is to print error message and sleep some minutes 
    until debugger is attached
  */
  wait_for_debugger(DEBUGGER_ATTACH_TIMEOUT);
#endif /* DEBUG_UNHANDLED_EXCEPTION_FILTER */
  __try
  {
    my_set_exception_pointers(ex_pointers);
    handle_fatal_signal(ex_pointers->ExceptionRecord->ExceptionCode);
  }
  __except(EXCEPTION_EXECUTE_HANDLER)
  {
    DWORD written;
    const char msg[] = "Got exception in exception handler!\n";
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE),msg, sizeof(msg)-1, 
      &written,NULL);
  }
  /*
    Return EXCEPTION_CONTINUE_SEARCH to give JIT debugger
    (drwtsn32 or vsjitdebugger) possibility to attach,
    if JIT debugger is configured.
    Windows Error reporting might generate a dump here.
  */
  return EXCEPTION_CONTINUE_SEARCH;
}


void init_signals(void)
{
   SetConsoleCtrlHandler(console_event_handler,TRUE);

   /* Avoid MessageBox()es*/
  _CrtSetReportMode(_CRT_WARN, _CRTDBG_MODE_FILE);
  _CrtSetReportFile(_CRT_WARN, _CRTDBG_FILE_STDERR);
  _CrtSetReportMode(_CRT_ERROR, _CRTDBG_MODE_FILE);
  _CrtSetReportFile(_CRT_ERROR, _CRTDBG_FILE_STDERR);
  _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
  _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);

   /*
     Do not use SEM_NOGPFAULTERRORBOX in the following SetErrorMode (),
     because it would prevent JIT debugger and Windows error reporting
     from working. We need WER or JIT-debugging, since our own unhandled
     exception filter is not guaranteed to work in all situation
     (like heap corruption or stack overflow)
   */
  SetErrorMode(SetErrorMode(0) | SEM_FAILCRITICALERRORS
                               | SEM_NOOPENFILEERRORBOX);
  if(!opt_debugging)
    SetUnhandledExceptionFilter(my_unhandler_exception_filter);
}


static void start_signal_handler(void)
{
#ifndef EMBEDDED_LIBRARY
  // Save vm id of this process
  if (!opt_bootstrap)
    create_pid_file();
#endif /* EMBEDDED_LIBRARY */
}


static void check_data_home(const char *path)
{}

#endif /* _WIN32 */


#if BACKTRACE_DEMANGLE
#include <cxxabi.h>
extern "C" char *my_demangle(const char *mangled_name, int *status)
{
  return abi::__cxa_demangle(mangled_name, NULL, NULL, status);
}
#endif


#ifdef DBUG_ASSERT_AS_PRINTF
extern "C" void
mariadb_dbug_assert_failed(const char *assert_expr, const char *file,
                           unsigned long line)
{
  fprintf(stderr, "Warning: assertion failed: %s at %s line %lu\n",
          assert_expr, file, line);
  if (opt_stack_trace)
  {
    fprintf(stderr, "Attempting backtrace to find out the reason for the assert:\n");
    my_print_stacktrace(NULL, (ulong) my_thread_stack_size, 1);
  }
}
#endif /* DBUG_ASSERT_AS_PRINT */

#if !defined(_WIN32)
#ifndef SA_RESETHAND
#define SA_RESETHAND 0
#endif /* SA_RESETHAND */
#ifndef SA_NODEFER
#define SA_NODEFER 0
#endif /* SA_NODEFER */

#ifndef EMBEDDED_LIBRARY

void init_signals(void)
{
  sigset_t set;
  struct sigaction sa;
  DBUG_ENTER("init_signals");

  my_sigset(THR_SERVER_ALARM,print_signal_warning); // Should never be called!

  if (opt_stack_trace || (test_flags & TEST_CORE_ON_SIGNAL))
  {
    sa.sa_flags = SA_RESETHAND | SA_NODEFER;
    sigemptyset(&sa.sa_mask);
    sigprocmask(SIG_SETMASK,&sa.sa_mask,NULL);

#if defined(__amiga__)
    sa.sa_handler=(void(*)())handle_fatal_signal;
#else
    sa.sa_handler=handle_fatal_signal;
#endif
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGABRT, &sa, NULL);
#ifdef SIGBUS
    sigaction(SIGBUS, &sa, NULL);
#endif
    sigaction(SIGILL, &sa, NULL);
    sigaction(SIGFPE, &sa, NULL);
  }

#ifdef HAVE_GETRLIMIT
  if (test_flags & TEST_CORE_ON_SIGNAL)
  {
    /* Change limits so that we will get a core file */
    STRUCT_RLIMIT rl;
    rl.rlim_cur = rl.rlim_max = (rlim_t) RLIM_INFINITY;
    if (setrlimit(RLIMIT_CORE, &rl) && global_system_variables.log_warnings)
      sql_print_warning("setrlimit could not change the size of core files to 'infinity';  We may not be able to generate a core file on signals");
  }
#endif
  (void) sigemptyset(&set);
  my_sigset(SIGPIPE,SIG_IGN);
  sigaddset(&set,SIGPIPE);
#ifndef IGNORE_SIGHUP_SIGQUIT
  sigaddset(&set,SIGQUIT);
  sigaddset(&set,SIGHUP);
#endif
  sigaddset(&set,SIGTERM);

  /* Fix signals if blocked by parents (can happen on Mac OS X) */
  sigemptyset(&sa.sa_mask);
  sa.sa_flags = 0;
  sa.sa_handler = print_signal_warning;
  sigaction(SIGTERM, &sa, (struct sigaction*) 0);
  sa.sa_flags = 0;
  sa.sa_handler = print_signal_warning;
  sigaction(SIGHUP, &sa, (struct sigaction*) 0);
  sigaddset(&set,THR_SERVER_ALARM);
  if (test_flags & TEST_SIGINT)
  {
    /* Allow SIGINT to break mysqld. This is for debugging with --gdb */
    my_sigset(SIGINT, end_mysqld_signal);
    sigdelset(&set, SIGINT);
  }
  else
  {
    sigaddset(&set,SIGINT);
#ifdef SIGTSTP
    sigaddset(&set,SIGTSTP);
#endif
  }

  sigprocmask(SIG_SETMASK,&set,NULL);
  pthread_sigmask(SIG_SETMASK,&set,NULL);
  DBUG_VOID_RETURN;
}


static void start_signal_handler(void)
{
  int error;
  pthread_attr_t thr_attr;
  DBUG_ENTER("start_signal_handler");

  (void) pthread_attr_init(&thr_attr);
  pthread_attr_setscope(&thr_attr,PTHREAD_SCOPE_SYSTEM);
  (void) pthread_attr_setdetachstate(&thr_attr,PTHREAD_CREATE_DETACHED);
  (void) my_setstacksize(&thr_attr,my_thread_stack_size);

  mysql_mutex_lock(&LOCK_start_thread);
  if (unlikely((error= mysql_thread_create(key_thread_signal_hand,
                                           &signal_thread, &thr_attr,
                                           signal_hand, 0))))
  {
    sql_print_error("Can't create interrupt-thread (error %d, errno: %d)",
		    error,errno);
    exit(1);
  }
  mysql_cond_wait(&COND_start_thread, &LOCK_start_thread);
  mysql_mutex_unlock(&LOCK_start_thread);

  (void) pthread_attr_destroy(&thr_attr);
  DBUG_VOID_RETURN;
}


#if defined(USE_ONE_SIGNAL_HAND)
pthread_handler_t kill_server_thread(void *arg __attribute__((unused)))
{
  my_thread_init();				// Initialize new thread
  break_connect_loop();
  my_thread_end();
  pthread_exit(0);
  return 0;
}
#endif


/** This threads handles all signals and alarms. */
/* ARGSUSED */
pthread_handler_t signal_hand(void *arg __attribute__((unused)))
{
  sigset_t set;
  int sig;
  my_thread_init();				// Init new thread
  DBUG_ENTER("signal_hand");
  signal_thread_in_use= 1;

  /*
    Setup alarm handler
    This should actually be '+ max_number_of_slaves' instead of +10,
    but the +10 should be quite safe.
  */
  init_thr_alarm(thread_scheduler->max_threads + extra_max_connections +
		 global_system_variables.max_insert_delayed_threads + 10);
  if (test_flags & TEST_SIGINT)
  {
    /* Allow SIGINT to break mysqld. This is for debugging with --gdb */
    (void) sigemptyset(&set);
    (void) sigaddset(&set,SIGINT);
    (void) pthread_sigmask(SIG_UNBLOCK,&set,NULL);
  }
  (void) sigemptyset(&set);			// Setup up SIGINT for debug
#ifdef USE_ONE_SIGNAL_HAND
  (void) sigaddset(&set,THR_SERVER_ALARM);	// For alarms
#endif
#ifndef IGNORE_SIGHUP_SIGQUIT
  (void) sigaddset(&set,SIGQUIT);
  (void) sigaddset(&set,SIGHUP);
#endif
  (void) sigaddset(&set,SIGTERM);
  (void) sigaddset(&set,SIGTSTP);

  /* Save pid to this process (or thread on Linux) */
  if (!opt_bootstrap)
    create_pid_file();

  /*
    signal to start_signal_handler that we are ready
    This works by waiting for start_signal_handler to free mutex,
    after which we signal it that we are ready.
    At this point there is no other threads running, so there
    should not be any other mysql_cond_signal() calls.
  */
  mysql_mutex_lock(&LOCK_start_thread);
  mysql_cond_broadcast(&COND_start_thread);
  mysql_mutex_unlock(&LOCK_start_thread);

  (void) pthread_sigmask(SIG_BLOCK,&set,NULL);
  for (;;)
  {
    int error;
    int origin;

    while ((error= my_sigwait(&set, &sig, &origin)) == EINTR) /* no-op */;
    if (cleanup_done)
    {
      DBUG_PRINT("quit",("signal_handler: calling my_thread_end()"));
      my_thread_end();
      DBUG_LEAVE;                               // Must match DBUG_ENTER()
      signal_thread_in_use= 0;
      pthread_exit(0);				// Safety
      return 0;                                 // Avoid compiler warnings
    }
    switch (sig) {
    case SIGTERM:
    case SIGQUIT:
    case SIGKILL:
#ifdef EXTRA_DEBUG
      sql_print_information("Got signal %d to shutdown server",sig);
#endif
      /* switch to the old log message processing */
      logger.set_handlers(global_system_variables.sql_log_slow ? LOG_FILE:LOG_NONE,
                          opt_log ? LOG_FILE:LOG_NONE);
      DBUG_PRINT("info",("Got signal: %d  abort_loop: %d",sig,abort_loop));
      if (!abort_loop)
      {
        /* Delete the instrumentation for the signal thread */
        PSI_CALL_delete_current_thread();
#ifdef USE_ONE_SIGNAL_HAND
	pthread_t tmp;
        if (unlikely((error= mysql_thread_create(0, /* Not instrumented */
                                                 &tmp, &connection_attrib,
                                                 kill_server_thread,
                                                 (void*) &sig))))
          sql_print_error("Can't create thread to kill server (errno= %d)",
                          error);
#else
        my_sigset(sig, SIG_IGN);
        break_connect_loop(); // MIT THREAD has a alarm thread
#endif
      }
      break;
    case SIGHUP:
#if defined(SI_KERNEL)
      if (!abort_loop && origin != SI_KERNEL)
#elif defined(SI_USER)
      if (!abort_loop && origin <= SI_USER)
#else
      if (!abort_loop)
#endif
      {
        int not_used;
	mysql_print_status();		// Print some debug info
	reload_acl_and_cache((THD*) 0,
			     (REFRESH_LOG | REFRESH_TABLES | REFRESH_FAST |
			      REFRESH_GRANT |
			      REFRESH_THREADS | REFRESH_HOSTS),
			     (TABLE_LIST*) 0, &not_used); // Flush logs

        /* reenable logs after the options were reloaded */
        ulonglong fixed_log_output_options=
          log_output_options & LOG_NONE ? LOG_TABLE : log_output_options;

        logger.set_handlers(global_system_variables.sql_log_slow
                            ? fixed_log_output_options : LOG_NONE,
                            opt_log ? fixed_log_output_options : LOG_NONE);
      }
      break;
#ifdef USE_ONE_SIGNAL_HAND
    case THR_SERVER_ALARM:
      process_alarm(sig);			// Trigger alarms.
      break;
#endif
    default:
#ifdef EXTRA_DEBUG
      sql_print_warning("Got signal: %d  error: %d",sig,error); /* purecov: tested */
#endif
      break;					/* purecov: tested */
    }
  }
  return(0);					/* purecov: deadcode */
}

static void check_data_home(const char *path)
{}

#endif /*!EMBEDDED_LIBRARY*/
#endif	/* _WIN32*/


/**
  All global error messages are sent here where the first one is stored
  for the client.
*/
/* ARGSUSED */
extern "C" void my_message_sql(uint error, const char *str, myf MyFlags);

void my_message_sql(uint error, const char *str, myf MyFlags)
{
  THD *thd= MyFlags & ME_ERROR_LOG_ONLY ? NULL : current_thd;
  Sql_condition::enum_warning_level level;
  sql_print_message_func func;
  DBUG_ENTER("my_message_sql");
  DBUG_PRINT("error", ("error: %u  message: '%s'  Flag: %lu", error, str,
                       MyFlags));

  DBUG_ASSERT(str != NULL);
  DBUG_ASSERT(error != 0);
  DBUG_ASSERT((MyFlags & ~(ME_BELL | ME_ERROR_LOG | ME_ERROR_LOG_ONLY |
                           ME_NOTE | ME_WARNING | ME_FATAL)) == 0);

  if (MyFlags & ME_NOTE)
  {
    level= Sql_condition::WARN_LEVEL_NOTE;
    func= sql_print_information;
  }
  else if (MyFlags & ME_WARNING)
  {
    level= Sql_condition::WARN_LEVEL_WARN;
    func= sql_print_warning;
  }
  else
  {
    level= Sql_condition::WARN_LEVEL_ERROR;
    func= sql_print_error;
  }

  if (likely(thd))
  {
    if (unlikely(MyFlags & ME_FATAL))
      thd->is_fatal_error= 1;
    (void) thd->raise_condition(error, "\0\0\0\0\0", level, str);
  }
  else
    mysql_audit_general(0, MYSQL_AUDIT_GENERAL_ERROR, error, str);

  /* When simulating OOM, skip writing to error log to avoid mtr errors */
  DBUG_EXECUTE_IF("simulate_out_of_memory", DBUG_VOID_RETURN;);

  if (unlikely(!thd) || thd->log_all_errors || (MyFlags & ME_ERROR_LOG))
    (*func)("%s: %s", my_progname_short, str); /* purecov: inspected */
  DBUG_VOID_RETURN;
}


extern "C" void *my_str_malloc_mysqld(size_t size);

void *my_str_malloc_mysqld(size_t size)
{
  return my_malloc(key_memory_my_str_malloc, size, MYF(MY_FAE));
}


#if 0
extern "C" void *my_str_realloc_mysqld(void *ptr, size_t size);
void *my_str_realloc_mysqld(void *ptr, size_t size)
{
  return my_realloc(key_memory_my_str_malloc, ptr, size, MYF(MY_FAE));
}
#endif




/**
  This function is used to check for stack overrun for pathological
  cases of  regular expressions and 'like' expressions.
*/
extern "C" int
check_enough_stack_size_slow()
{
  uchar stack_top;
  THD *my_thd= current_thd;
  if (my_thd != NULL)
    return check_stack_overrun(my_thd, STACK_MIN_SIZE * 2, &stack_top);
  return 0;
}


/*
  The call to current_thd in check_enough_stack_size_slow is quite expensive,
  so we try to avoid it for the normal cases.
  The size of  each stack frame for the wildcmp() routines is ~128 bytes,
  so checking  *every* recursive call is not necessary.
 */
extern "C" int
check_enough_stack_size(int recurse_level)
{
  if (recurse_level % 16 != 0)
    return 0;
  return check_enough_stack_size_slow();
}


static void init_libstrings()
{
#ifndef EMBEDDED_LIBRARY
  my_string_stack_guard= check_enough_stack_size;
#endif
}


/**
  Initialize one of the global date/time format variables.

  @param format_type		What kind of format should be supported
  @param var_ptr		Pointer to variable that should be updated

  @retval
    0 ok
  @retval
    1 error
*/

static bool init_global_datetime_format(timestamp_type format_type,
                                        DATE_TIME_FORMAT *format)
{
  /*
    Get command line option
    format->format.str is already set by my_getopt
  */
  format->format.length= strlen(format->format.str);

  if (parse_date_time_format(format_type, format))
  {
    fprintf(stderr, "Wrong date/time format specifier: %s\n",
            format->format.str);
    return true;
  }
  return false;
}

#define COM_STATUS(X)  (void*) offsetof(STATUS_VAR, X), SHOW_LONG_STATUS
#define STMT_STATUS(X) COM_STATUS(com_stat[(uint) X])

SHOW_VAR com_status_vars[]= {
  {"admin_commands",       COM_STATUS(com_other)},
  {"alter_db",             STMT_STATUS(SQLCOM_ALTER_DB)},
  {"alter_db_upgrade",     STMT_STATUS(SQLCOM_ALTER_DB_UPGRADE)},
  {"alter_event",          STMT_STATUS(SQLCOM_ALTER_EVENT)},
  {"alter_function",       STMT_STATUS(SQLCOM_ALTER_FUNCTION)},
  {"alter_procedure",      STMT_STATUS(SQLCOM_ALTER_PROCEDURE)},
  {"alter_server",         STMT_STATUS(SQLCOM_ALTER_SERVER)},
  {"alter_sequence",       STMT_STATUS(SQLCOM_ALTER_SEQUENCE)},
  {"alter_table",          STMT_STATUS(SQLCOM_ALTER_TABLE)},
  {"alter_user",           STMT_STATUS(SQLCOM_ALTER_USER)},
  {"analyze",              STMT_STATUS(SQLCOM_ANALYZE)},
  {"assign_to_keycache",   STMT_STATUS(SQLCOM_ASSIGN_TO_KEYCACHE)},
  {"backup",               STMT_STATUS(SQLCOM_BACKUP)},
  {"backup_lock",          STMT_STATUS(SQLCOM_BACKUP_LOCK)},
  {"begin",                STMT_STATUS(SQLCOM_BEGIN)},
  {"binlog",               STMT_STATUS(SQLCOM_BINLOG_BASE64_EVENT)},
  {"call_procedure",       STMT_STATUS(SQLCOM_CALL)},
  {"change_db",            STMT_STATUS(SQLCOM_CHANGE_DB)},
  {"change_master",        STMT_STATUS(SQLCOM_CHANGE_MASTER)},
  {"check",                STMT_STATUS(SQLCOM_CHECK)},
  {"checksum",             STMT_STATUS(SQLCOM_CHECKSUM)},
  {"commit",               STMT_STATUS(SQLCOM_COMMIT)},
  {"compound_sql",         STMT_STATUS(SQLCOM_COMPOUND)},
  {"create_db",            STMT_STATUS(SQLCOM_CREATE_DB)},
  {"create_event",         STMT_STATUS(SQLCOM_CREATE_EVENT)},
  {"create_function",      STMT_STATUS(SQLCOM_CREATE_SPFUNCTION)},
  {"create_index",         STMT_STATUS(SQLCOM_CREATE_INDEX)},
  {"create_package",       STMT_STATUS(SQLCOM_CREATE_PACKAGE)},
  {"create_package_body",  STMT_STATUS(SQLCOM_CREATE_PACKAGE_BODY)},
  {"create_procedure",     STMT_STATUS(SQLCOM_CREATE_PROCEDURE)},
  {"create_role",          STMT_STATUS(SQLCOM_CREATE_ROLE)},
  {"create_sequence",      STMT_STATUS(SQLCOM_CREATE_SEQUENCE)},
  {"create_server",        STMT_STATUS(SQLCOM_CREATE_SERVER)},
  {"create_table",         STMT_STATUS(SQLCOM_CREATE_TABLE)},
  {"create_temporary_table", COM_STATUS(com_create_tmp_table)},
  {"create_trigger",       STMT_STATUS(SQLCOM_CREATE_TRIGGER)},
  {"create_udf",           STMT_STATUS(SQLCOM_CREATE_FUNCTION)},
  {"create_user",          STMT_STATUS(SQLCOM_CREATE_USER)},
  {"create_view",          STMT_STATUS(SQLCOM_CREATE_VIEW)},
  {"dealloc_sql",          STMT_STATUS(SQLCOM_DEALLOCATE_PREPARE)},
  {"delete",               STMT_STATUS(SQLCOM_DELETE)},
  {"delete_multi",         STMT_STATUS(SQLCOM_DELETE_MULTI)},
  {"do",                   STMT_STATUS(SQLCOM_DO)},
  {"drop_db",              STMT_STATUS(SQLCOM_DROP_DB)},
  {"drop_event",           STMT_STATUS(SQLCOM_DROP_EVENT)},
  {"drop_function",        STMT_STATUS(SQLCOM_DROP_FUNCTION)},
  {"drop_index",           STMT_STATUS(SQLCOM_DROP_INDEX)},
  {"drop_procedure",       STMT_STATUS(SQLCOM_DROP_PROCEDURE)},
  {"drop_package",         STMT_STATUS(SQLCOM_DROP_PACKAGE)},
  {"drop_package_body",    STMT_STATUS(SQLCOM_DROP_PACKAGE_BODY)},
  {"drop_role",            STMT_STATUS(SQLCOM_DROP_ROLE)},
  {"drop_server",          STMT_STATUS(SQLCOM_DROP_SERVER)},
  {"drop_sequence",        STMT_STATUS(SQLCOM_DROP_SEQUENCE)},
  {"drop_table",           STMT_STATUS(SQLCOM_DROP_TABLE)},
  {"drop_temporary_table", COM_STATUS(com_drop_tmp_table)},
  {"drop_trigger",         STMT_STATUS(SQLCOM_DROP_TRIGGER)},
  {"drop_user",            STMT_STATUS(SQLCOM_DROP_USER)},
  {"drop_view",            STMT_STATUS(SQLCOM_DROP_VIEW)},
  {"empty_query",          STMT_STATUS(SQLCOM_EMPTY_QUERY)},
  {"execute_immediate",    STMT_STATUS(SQLCOM_EXECUTE_IMMEDIATE)},
  {"execute_sql",          STMT_STATUS(SQLCOM_EXECUTE)},
  {"flush",                STMT_STATUS(SQLCOM_FLUSH)},
  {"get_diagnostics",      STMT_STATUS(SQLCOM_GET_DIAGNOSTICS)},
  {"grant",                STMT_STATUS(SQLCOM_GRANT)},
  {"grant_role",           STMT_STATUS(SQLCOM_GRANT_ROLE)},
  {"ha_close",             STMT_STATUS(SQLCOM_HA_CLOSE)},
  {"ha_open",              STMT_STATUS(SQLCOM_HA_OPEN)},
  {"ha_read",              STMT_STATUS(SQLCOM_HA_READ)},
  {"help",                 STMT_STATUS(SQLCOM_HELP)},
  {"insert",               STMT_STATUS(SQLCOM_INSERT)},
  {"insert_select",        STMT_STATUS(SQLCOM_INSERT_SELECT)},
  {"install_plugin",       STMT_STATUS(SQLCOM_INSTALL_PLUGIN)},
  {"kill",                 STMT_STATUS(SQLCOM_KILL)},
  {"load",                 STMT_STATUS(SQLCOM_LOAD)},
  {"lock_tables",          STMT_STATUS(SQLCOM_LOCK_TABLES)},
  {"optimize",             STMT_STATUS(SQLCOM_OPTIMIZE)},
  {"preload_keys",         STMT_STATUS(SQLCOM_PRELOAD_KEYS)},
  {"prepare_sql",          STMT_STATUS(SQLCOM_PREPARE)},
  {"purge",                STMT_STATUS(SQLCOM_PURGE)},
  {"purge_before_date",    STMT_STATUS(SQLCOM_PURGE_BEFORE)},
  {"release_savepoint",    STMT_STATUS(SQLCOM_RELEASE_SAVEPOINT)},
  {"rename_table",         STMT_STATUS(SQLCOM_RENAME_TABLE)},
  {"rename_user",          STMT_STATUS(SQLCOM_RENAME_USER)},
  {"repair",               STMT_STATUS(SQLCOM_REPAIR)},
  {"replace",              STMT_STATUS(SQLCOM_REPLACE)},
  {"replace_select",       STMT_STATUS(SQLCOM_REPLACE_SELECT)},
  {"reset",                STMT_STATUS(SQLCOM_RESET)},
  {"resignal",             STMT_STATUS(SQLCOM_RESIGNAL)},
  {"revoke",               STMT_STATUS(SQLCOM_REVOKE)},
  {"revoke_all",           STMT_STATUS(SQLCOM_REVOKE_ALL)},
  {"revoke_role",          STMT_STATUS(SQLCOM_REVOKE_ROLE)},
  {"rollback",             STMT_STATUS(SQLCOM_ROLLBACK)},
  {"rollback_to_savepoint",STMT_STATUS(SQLCOM_ROLLBACK_TO_SAVEPOINT)},
  {"savepoint",            STMT_STATUS(SQLCOM_SAVEPOINT)},
  {"select",               STMT_STATUS(SQLCOM_SELECT)},
  {"set_option",           STMT_STATUS(SQLCOM_SET_OPTION)},
  {"show_authors",         STMT_STATUS(SQLCOM_SHOW_AUTHORS)},
  {"show_binlog_events",   STMT_STATUS(SQLCOM_SHOW_BINLOG_EVENTS)},
  {"show_binlogs",         STMT_STATUS(SQLCOM_SHOW_BINLOGS)},
  {"show_charsets",        STMT_STATUS(SQLCOM_SHOW_CHARSETS)},
  {"show_collations",      STMT_STATUS(SQLCOM_SHOW_COLLATIONS)},
  {"show_contributors",    STMT_STATUS(SQLCOM_SHOW_CONTRIBUTORS)},
  {"show_create_db",       STMT_STATUS(SQLCOM_SHOW_CREATE_DB)},
  {"show_create_event",    STMT_STATUS(SQLCOM_SHOW_CREATE_EVENT)},
  {"show_create_func",     STMT_STATUS(SQLCOM_SHOW_CREATE_FUNC)},
  {"show_create_package",  STMT_STATUS(SQLCOM_SHOW_CREATE_PACKAGE)},
  {"show_create_package_body",STMT_STATUS(SQLCOM_SHOW_CREATE_PACKAGE_BODY)},
  {"show_create_proc",     STMT_STATUS(SQLCOM_SHOW_CREATE_PROC)},
  {"show_create_table",    STMT_STATUS(SQLCOM_SHOW_CREATE)},
  {"show_create_trigger",  STMT_STATUS(SQLCOM_SHOW_CREATE_TRIGGER)},
  {"show_create_user",     STMT_STATUS(SQLCOM_SHOW_CREATE_USER)},
  {"show_databases",       STMT_STATUS(SQLCOM_SHOW_DATABASES)},
  {"show_engine_logs",     STMT_STATUS(SQLCOM_SHOW_ENGINE_LOGS)},
  {"show_engine_mutex",    STMT_STATUS(SQLCOM_SHOW_ENGINE_MUTEX)},
  {"show_engine_status",   STMT_STATUS(SQLCOM_SHOW_ENGINE_STATUS)},
  {"show_errors",          STMT_STATUS(SQLCOM_SHOW_ERRORS)},
  {"show_events",          STMT_STATUS(SQLCOM_SHOW_EVENTS)},
  {"show_explain",         STMT_STATUS(SQLCOM_SHOW_EXPLAIN)},
  {"show_analyze",         STMT_STATUS(SQLCOM_SHOW_ANALYZE)},
  {"show_fields",          STMT_STATUS(SQLCOM_SHOW_FIELDS)},
#ifndef DBUG_OFF
  {"show_function_code",   STMT_STATUS(SQLCOM_SHOW_FUNC_CODE)},
#endif
  {"show_function_status", STMT_STATUS(SQLCOM_SHOW_STATUS_FUNC)},
  {"show_generic",         STMT_STATUS(SQLCOM_SHOW_GENERIC)},
  {"show_grants",          STMT_STATUS(SQLCOM_SHOW_GRANTS)},
  {"show_keys",            STMT_STATUS(SQLCOM_SHOW_KEYS)},
  {"show_binlog_status",   STMT_STATUS(SQLCOM_SHOW_BINLOG_STAT)},
  {"show_open_tables",     STMT_STATUS(SQLCOM_SHOW_OPEN_TABLES)},
  {"show_package_status",  STMT_STATUS(SQLCOM_SHOW_STATUS_PACKAGE)},
#ifndef DBUG_OFF
  {"show_package_body_code",   STMT_STATUS(SQLCOM_SHOW_PACKAGE_BODY_CODE)},
#endif
  {"show_package_body_status", STMT_STATUS(SQLCOM_SHOW_STATUS_PACKAGE_BODY)},
  {"show_plugins",         STMT_STATUS(SQLCOM_SHOW_PLUGINS)},
  {"show_privileges",      STMT_STATUS(SQLCOM_SHOW_PRIVILEGES)},
#ifndef DBUG_OFF
  {"show_procedure_code",  STMT_STATUS(SQLCOM_SHOW_PROC_CODE)},
#endif
  {"show_procedure_status",STMT_STATUS(SQLCOM_SHOW_STATUS_PROC)},
  {"show_processlist",     STMT_STATUS(SQLCOM_SHOW_PROCESSLIST)},
  {"show_profile",         STMT_STATUS(SQLCOM_SHOW_PROFILE)},
  {"show_profiles",        STMT_STATUS(SQLCOM_SHOW_PROFILES)},
  {"show_relaylog_events", STMT_STATUS(SQLCOM_SHOW_RELAYLOG_EVENTS)},
  {"show_slave_hosts",     STMT_STATUS(SQLCOM_SHOW_SLAVE_HOSTS)},
  {"show_slave_status",    STMT_STATUS(SQLCOM_SHOW_SLAVE_STAT)},
  {"show_status",          STMT_STATUS(SQLCOM_SHOW_STATUS)},
  {"show_storage_engines", STMT_STATUS(SQLCOM_SHOW_STORAGE_ENGINES)},
  {"show_table_status",    STMT_STATUS(SQLCOM_SHOW_TABLE_STATUS)},
  {"show_tables",          STMT_STATUS(SQLCOM_SHOW_TABLES)},
  {"show_triggers",        STMT_STATUS(SQLCOM_SHOW_TRIGGERS)},
  {"show_variables",       STMT_STATUS(SQLCOM_SHOW_VARIABLES)},
  {"show_warnings",        STMT_STATUS(SQLCOM_SHOW_WARNS)},
  {"shutdown",             STMT_STATUS(SQLCOM_SHUTDOWN)},
  {"signal",               STMT_STATUS(SQLCOM_SIGNAL)},
  {"start_all_slaves",     STMT_STATUS(SQLCOM_SLAVE_ALL_START)},
  {"start_slave",          STMT_STATUS(SQLCOM_SLAVE_START)},
  {"stmt_close",           COM_STATUS(com_stmt_close)},
  {"stmt_execute",         COM_STATUS(com_stmt_execute)},
  {"stmt_fetch",           COM_STATUS(com_stmt_fetch)},
  {"stmt_prepare",         COM_STATUS(com_stmt_prepare)},
  {"stmt_reprepare",       COM_STATUS(com_stmt_reprepare)},
  {"stmt_reset",           COM_STATUS(com_stmt_reset)},
  {"stmt_send_long_data",  COM_STATUS(com_stmt_send_long_data)},
  {"stop_all_slaves",      STMT_STATUS(SQLCOM_SLAVE_ALL_STOP)},
  {"stop_slave",           STMT_STATUS(SQLCOM_SLAVE_STOP)},
  {"truncate",             STMT_STATUS(SQLCOM_TRUNCATE)},
  {"uninstall_plugin",     STMT_STATUS(SQLCOM_UNINSTALL_PLUGIN)},
  {"unlock_tables",        STMT_STATUS(SQLCOM_UNLOCK_TABLES)},
  {"update",               STMT_STATUS(SQLCOM_UPDATE)},
  {"update_multi",         STMT_STATUS(SQLCOM_UPDATE_MULTI)},
  {"xa_commit",            STMT_STATUS(SQLCOM_XA_COMMIT)},
  {"xa_end",               STMT_STATUS(SQLCOM_XA_END)},
  {"xa_prepare",           STMT_STATUS(SQLCOM_XA_PREPARE)},
  {"xa_recover",           STMT_STATUS(SQLCOM_XA_RECOVER)},
  {"xa_rollback",          STMT_STATUS(SQLCOM_XA_ROLLBACK)},
  {"xa_start",             STMT_STATUS(SQLCOM_XA_START)},
  {NullS, NullS, SHOW_LONG}
};


#ifdef HAVE_PSI_STATEMENT_INTERFACE
PSI_statement_info sql_statement_info[(uint) SQLCOM_END + 1];
PSI_statement_info com_statement_info[(uint) COM_END + 1];

/**
  Initialize the command names array.
  Since we do not want to maintain a separate array,
  this is populated from data mined in com_status_vars,
  which already has one name for each command.
*/
void init_sql_statement_info()
{
  size_t first_com= offsetof(STATUS_VAR, com_stat[0]);
  size_t last_com=  offsetof(STATUS_VAR, com_stat[(uint) SQLCOM_END]);
  int record_size= offsetof(STATUS_VAR, com_stat[1])
                   - offsetof(STATUS_VAR, com_stat[0]);
  size_t ptr;
  uint i;
  uint com_index;

  static const char* dummy= "";
  for (i= 0; i < ((uint) SQLCOM_END + 1); i++)
  {
    sql_statement_info[i].m_name= dummy;
    sql_statement_info[i].m_flags= 0;
  }

  SHOW_VAR *var= &com_status_vars[0];
  while (var->name != NULL)
  {
    ptr= (size_t)(var->value);
    if ((first_com <= ptr) && (ptr < last_com))
    {
      com_index= ((int)(ptr - first_com))/record_size;
      DBUG_ASSERT(com_index < (uint) SQLCOM_END);
      sql_statement_info[com_index].m_name= var->name;
    }
    var++;
  }

  DBUG_ASSERT(strcmp(sql_statement_info[(uint) SQLCOM_SELECT].m_name, "select") == 0);
  DBUG_ASSERT(strcmp(sql_statement_info[(uint) SQLCOM_SIGNAL].m_name, "signal") == 0);

  sql_statement_info[(uint) SQLCOM_END].m_name= "error";
}

void init_com_statement_info()
{
  uint index;

  for (index= 0; index < (uint) COM_END + 1; index++)
  {
    com_statement_info[index].m_name= command_name[index].str;
    com_statement_info[index].m_flags= 0;
  }

  /* "statement/abstract/query" can mutate into "statement/sql/..." */
  com_statement_info[(uint) COM_QUERY].m_flags= PSI_FLAG_MUTABLE;
}
#endif


#ifdef SAFEMALLOC
/*
  Return the id for the current THD, to allow safemalloc to associate
  the memory with the right id.
*/

extern "C" my_thread_id mariadb_dbug_id()
{
  THD *thd;
  if ((thd= current_thd) && thd->thread_dbug_id)
  {
    return thd->thread_dbug_id;
  }
  return my_thread_dbug_id();
}
#endif /* SAFEMALLOC */

/* Thread Mem Usage By P.Linux */
extern "C" {
static void my_malloc_size_cb_func(long long size, my_bool is_thread_specific)
{
  THD *thd= current_thd;

#ifndef DBUG_OFF
  statistic_increment(malloc_calls, &LOCK_status);
#endif

  /*
    When thread specific is set, both mysqld_server_initialized and thd
    must be set, and we check that with DBUG_ASSERT.

    However, do not crash, if current_thd is NULL, in release version.
  */
  DBUG_ASSERT(!is_thread_specific || (mysqld_server_initialized && thd));

  if (is_thread_specific && likely(thd))  /* If thread specific memory */
  {
    DBUG_PRINT("info", ("thd memory_used: %lld  size: %lld",
                        (longlong) thd->status_var.local_memory_used,
                        size));
    thd->status_var.local_memory_used+= size;
    set_if_bigger(thd->status_var.max_local_memory_used,
                  thd->status_var.local_memory_used);
    if (size > 0 &&
        thd->status_var.local_memory_used > (int64)thd->variables.max_mem_used &&
        likely(!thd->killed) && !thd->get_stmt_da()->is_set())
    {
      /* Ensure we don't get called here again */
      char buf[50], *buf2;
      thd->set_killed(KILL_QUERY);
      my_snprintf(buf, sizeof(buf), "--max-session-mem-used=%llu",
                  thd->variables.max_mem_used);
      if ((buf2= (char*) thd->alloc(256)))
      {
        my_snprintf(buf2, 256, ER_THD(thd, ER_OPTION_PREVENTS_STATEMENT), buf);
        thd->set_killed(KILL_QUERY, ER_OPTION_PREVENTS_STATEMENT, buf2);
      }
      else
      {
        thd->set_killed(KILL_QUERY, ER_OPTION_PREVENTS_STATEMENT,
                        "--max-session-mem-used");
      }
    }
    DBUG_ASSERT((longlong) thd->status_var.local_memory_used >= 0 ||
                !debug_assert_on_not_freed_memory);
  }
  else if (likely(thd))
  {
    DBUG_PRINT("info", ("global thd memory_used: %lld  size: %lld",
                        (longlong) thd->status_var.global_memory_used, size));
    thd->status_var.global_memory_used+= size;
  }
  else
    update_global_memory_status(size);
}

int json_escape_string(const char *str,const char *str_end,
                       char *json, char *json_end)
{
  return json_escape(system_charset_info,
                     (const uchar *) str, (const uchar *) str_end,
                     &my_charset_utf8mb4_bin,
                     (uchar *) json, (uchar *) json_end);
}


int json_unescape_json(const char *json_str, const char *json_end,
                       char *res, char *res_end)
{
  return json_unescape(&my_charset_utf8mb4_bin,
                       (const uchar *) json_str, (const uchar *) json_end,
                       system_charset_info, (uchar *) res, (uchar *) res_end);
}

} /*extern "C"*/


/**
  Create a replication file name or base for file names.

  @param[in] opt Value of option, or NULL
  @param[in] def Default value if option value is not set.
  @param[in] ext Extension to use for the path

  @returns Pointer to string containing the full file path, or NULL if
  it was not possible to create the path.
 */
static const char *rpl_make_log_name(PSI_memory_key key, const char *opt,
                                     const char *def, const char *ext)
{
  DBUG_ENTER("rpl_make_log_name");
  DBUG_PRINT("enter", ("opt: %s, def: %s, ext: %s", opt ? opt : "(null)",
                       def, ext));
  char buff[FN_REFLEN];
  const char *base= opt ? opt : def;
  unsigned int options=
    MY_REPLACE_EXT | MY_UNPACK_FILENAME | MY_SAFE_PATH;

  /* mysql_real_data_home_ptr  may be null if no value of datadir has been
     specified through command-line or througha cnf file. If that is the
     case we make mysql_real_data_home_ptr point to mysql_real_data_home
     which, in that case holds the default path for data-dir.
  */
  if(mysql_real_data_home_ptr == NULL)
    mysql_real_data_home_ptr= mysql_real_data_home;

  if (fn_format(buff, base, mysql_real_data_home_ptr, ext, options))
    DBUG_RETURN(my_strdup(key, buff, MYF(MY_WME)));
  else
    DBUG_RETURN(NULL);
}

/* We have to setup my_malloc_size_cb_func early to catch all mallocs */

static int init_early_variables()
{
  set_current_thd(0);
  set_malloc_size_cb(my_malloc_size_cb_func);
  global_status_var.global_memory_used= 0;
  init_alloc_root(PSI_NOT_INSTRUMENTED, &startup_root, 1024, 0, MYF(0));
  init_alloc_root(PSI_NOT_INSTRUMENTED, &read_only_root, 1024, 0,
		  MYF(MY_ROOT_USE_MPROTECT));
  return 0;
}

#ifdef _WIN32
static void get_win_tzname(char* buf, size_t size)
{
  static struct
  {
    const wchar_t* windows_name;
    const char*  tzdb_name;
  }
  tz_data[] =
  {
#include "win_tzname_data.h"
    {0,0}
  };
  DYNAMIC_TIME_ZONE_INFORMATION  tzinfo;
  if (GetDynamicTimeZoneInformation(&tzinfo) == TIME_ZONE_ID_INVALID)
  {
    strncpy(buf, "unknown", size);
    return;
  }

  for (size_t i= 0; tz_data[i].windows_name; i++)
  {
    if (wcscmp(tzinfo.TimeZoneKeyName, tz_data[i].windows_name) == 0)
    {
      strncpy(buf, tz_data[i].tzdb_name, size);
      return;
    }
  }
  wcstombs(buf, tzinfo.TimeZoneKeyName, size);
  buf[size-1]= 0;
  return;
}
#endif

static int init_common_variables()
{
  umask(((~my_umask) & 0666));
  connection_errors_select= 0;
  connection_errors_accept= 0;
  connection_errors_tcpwrap= 0;
  connection_errors_internal= 0;
  connection_errors_max_connection= 0;
  connection_errors_peer_addr= 0;
  my_decimal_set_zero(&decimal_zero); // set decimal_zero constant;

  init_libstrings();
  tzset();			// Set tzname

#ifdef SAFEMALLOC
  sf_malloc_dbug_id= mariadb_dbug_id;
#endif /* SAFEMALLOC */
#ifdef DBUG_ASSERT_AS_PRINTF
  my_dbug_assert_failed= mariadb_dbug_assert_failed;
#endif /* DBUG_ASSERT_AS_PRINTF */

  if (!(type_handler_data= new Type_handler_data) ||
      type_handler_data->init())
  {
    sql_perror("Could not allocate type_handler_data");
    return 1;
  }

  max_system_variables.pseudo_thread_id= ~(my_thread_id) 0;
  server_start_time= flush_status_time= my_time(0);
  my_disable_copystat_in_redel= 1;

  global_rpl_filter= new Rpl_filter;
  binlog_filter= new Rpl_filter;
  if (!global_rpl_filter || !binlog_filter)
  {
    sql_perror("Could not allocate replication and binlog filters");
    exit(1);
  }

#ifdef HAVE_OPENSSL
  if (check_openssl_compatibility())
  {
    sql_print_error("Incompatible OpenSSL version. Cannot continue...");
    exit(1);
  }
#endif

  if (init_thread_environment() || mysql_init_variables())
    exit(1);

  if (ignore_db_dirs_init())
    exit(1);

#ifdef _WIN32
  get_win_tzname(system_time_zone, sizeof(system_time_zone));
#elif defined(HAVE_TZNAME)
  struct tm tm_tmp;
  localtime_r(&server_start_time,&tm_tmp);
  const char *tz_name=  tzname[tm_tmp.tm_isdst != 0 ? 1 : 0];
  strmake_buf(system_time_zone, tz_name);
#endif /* HAVE_TZNAME */

  /*
    We set SYSTEM time zone as reasonable default and
    also for failure of my_tz_init() and bootstrap mode.
    If user explicitly set time zone with --default-time-zone
    option we will change this value in my_tz_init().
  */
  global_system_variables.time_zone= my_tz_SYSTEM;

#ifdef HAVE_PSI_INTERFACE
  /*
    Complete the mysql_bin_log initialization.
    Instrumentation keys are known only after the performance schema
    initialization, and can not be set in the MYSQL_BIN_LOG
    constructor (called before main()).
  */
  mysql_bin_log.set_psi_keys(key_BINLOG_LOCK_index,
                             key_BINLOG_COND_relay_log_updated,
                             key_BINLOG_COND_bin_log_updated,
                             key_file_binlog,
                             key_file_binlog_cache,
                             key_file_binlog_index,
                             key_file_binlog_index_cache,
                             key_BINLOG_COND_queue_busy,
                             key_LOCK_binlog_end_pos);
#endif

  /*
    Init mutexes for the global MYSQL_BIN_LOG objects.
    As safe_mutex depends on what MY_INIT() does, we can't init the mutexes of
    global MYSQL_BIN_LOGs in their constructors, because then they would be
    inited before MY_INIT(). So we do it here.
  */
  mysql_bin_log.init_pthread_objects();

  /* TODO: remove this when my_time_t is 64 bit compatible */
  if (!IS_TIME_T_VALID_FOR_TIMESTAMP(server_start_time))
  {
    sql_print_error("This server doesn't support dates later than 2038");
    exit(1);
  }

  opt_log_basename= const_cast<char *>("mysql");

  if (gethostname(glob_hostname,sizeof(glob_hostname)) < 0)
  {
    /*
      Get hostname of computer (used by 'show variables') and as default
      basename for the pid file if --log-basename is not given.
    */
    strmake(glob_hostname, STRING_WITH_LEN("localhost"));
    sql_print_warning("gethostname failed, using '%s' as hostname",
                        glob_hostname);
  }
  else if (is_filename_allowed(glob_hostname, strlen(glob_hostname), FALSE))
    opt_log_basename= glob_hostname;

  strmake(pidfile_name, opt_log_basename, sizeof(pidfile_name)-5);
  strmov(fn_ext(pidfile_name),".pid");		// Add proper extension
  SYSVAR_AUTOSIZE(pidfile_name_ptr, pidfile_name);
  set_sys_var_value_origin(&opt_tc_log_size, sys_var::AUTO);

  /*
    The default-storage-engine entry in my_long_options should have a
    non-null default value. It was earlier intialized as
    (longlong)"MyISAM" in my_long_options but this triggered a
    compiler error in the Sun Studio 12 compiler. As a work-around we
    set the def_value member to 0 in my_long_options and initialize it
    to the correct value here.

    From MySQL 5.5 onwards, the default storage engine is InnoDB
    (except in the embedded server, where the default continues to
    be MyISAM)
  */
#if defined(WITH_INNOBASE_STORAGE_ENGINE)
  default_storage_engine= const_cast<char *>("InnoDB");
#else
  default_storage_engine= const_cast<char *>("MyISAM");
#endif
  default_tmp_storage_engine= NULL;
  gtid_pos_auto_engines= const_cast<char *>("");

  /*
    Add server status variables to the dynamic list of
    status variables that is shown by SHOW STATUS.
    Later, in plugin_init, and mysql_install_plugin
    new entries could be added to that list.
  */
  if (add_status_vars(status_vars))
    exit(1); // an error was already reported

#ifndef DBUG_OFF
  /*
    We have few debug-only commands in com_status_vars, only visible in debug
    builds. for simplicity we enable the assert only in debug builds

    There are 10 Com_ variables which don't have corresponding SQLCOM_ values:
    (TODO strictly speaking they shouldn't be here, should not have Com_ prefix
    that is. Perhaps Stmt_ ? Comstmt_ ? Prepstmt_ ?)

      Com_admin_commands         => com_other
      Com_create_temporary_table => com_create_tmp_table
      Com_drop_temporary_table   => com_drop_tmp_table
      Com_stmt_close             => com_stmt_close
      Com_stmt_execute           => com_stmt_execute
      Com_stmt_fetch             => com_stmt_fetch
      Com_stmt_prepare           => com_stmt_prepare
      Com_stmt_reprepare         => com_stmt_reprepare
      Com_stmt_reset             => com_stmt_reset
      Com_stmt_send_long_data    => com_stmt_send_long_data

    With this correction the number of Com_ variables (number of elements in
    the array, excluding the last element - terminator) must match the number
    of SQLCOM_ constants.
  */
  compile_time_assert(sizeof(com_status_vars)/sizeof(com_status_vars[0]) - 1 ==
                     SQLCOM_END + 10);
#endif

  if (get_options(&remaining_argc, &remaining_argv))
    exit(1);
  if (IS_SYSVAR_AUTOSIZE(&server_version_ptr))
    set_server_version(server_version, sizeof(server_version));

  mysql_real_data_home_len= uint(strlen(mysql_real_data_home));

  if (!opt_abort)
  {
    if (IS_SYSVAR_AUTOSIZE(&server_version_ptr))
      sql_print_information("%s (server %s) starting as process %lu ...",
                            my_progname, server_version, (ulong) getpid());
    else
    {
      char real_server_version[SERVER_VERSION_LENGTH];
      set_server_version(real_server_version, sizeof(real_server_version));
      sql_print_information("%s (server %s as %s) starting as process %lu ...",
                            my_progname, real_server_version, server_version,
                            (ulong) getpid());
    }
  }

  sf_leaking_memory= 0; // no memory leaks from now on

#ifndef EMBEDDED_LIBRARY
  if (opt_abort && !opt_verbose)
    unireg_abort(0);
#endif /*!EMBEDDED_LIBRARY*/

  DBUG_PRINT("info",("%s  Ver %s for %s on %s\n",my_progname,
		     server_version, SYSTEM_TYPE,MACHINE_TYPE));

  /* Initialize large page size */
  if (opt_large_pages)
  {
    DBUG_PRINT("info", ("Large page set"));
    if (my_init_large_pages(opt_super_large_pages))
    {
      return 1;
    }
  }

#if defined(HAVE_POOL_OF_THREADS)
  if (IS_SYSVAR_AUTOSIZE(&threadpool_size))
    SYSVAR_AUTOSIZE(threadpool_size, my_getncpus());
#endif

  /* connections and databases needs lots of files */
  {
    uint files, wanted_files, max_open_files, min_tc_size, extra_files,
      min_connections;
    ulong org_max_connections, org_tc_size;

    /* Number of files reserved for temporary files */
    extra_files= 30;
    min_connections= 10;
    /* MyISAM requires two file handles per table. */
    wanted_files= (extra_files + max_connections + extra_max_connections +
                   tc_size * 2 * tc_instances);
#if defined(HAVE_POOL_OF_THREADS) && !defined(_WIN32)
    // add epoll or kevent fd for each threadpool group, in case pool of threads is used
    wanted_files+= (thread_handling > SCHEDULER_NO_THREADS) ? 0 : threadpool_size;
#endif

    min_tc_size= MY_MIN(tc_size, TABLE_OPEN_CACHE_MIN);
    org_max_connections= max_connections;
    org_tc_size= tc_size;

    /*
      We are trying to allocate no less than max_connections*5 file
      handles (i.e. we are trying to set the limit so that they will
      be available).  In addition, we allocate no less than how much
      was already allocated.  However below we report a warning and
      recompute values only if we got less file handles than were
      explicitly requested.  No warning and re-computation occur if we
      can't get max_connections*5 but still got no less than was
      requested (value of wanted_files).
    */
    max_open_files= MY_MAX(MY_MAX(wanted_files,
                                  (max_connections + extra_max_connections)*5),
                           open_files_limit);
    files= my_set_max_open_files(max_open_files);
    SYSVAR_AUTOSIZE_IF_CHANGED(open_files_limit, files, ulong);

    if (files < wanted_files && global_system_variables.log_warnings)
      sql_print_warning("Could not increase number of max_open_files to more than %u (request: %u)", files, wanted_files);

    /* If we required too much tc_instances than we reduce */
    SYSVAR_AUTOSIZE_IF_CHANGED(tc_instances,
                               (uint32) MY_MIN(MY_MAX((files - extra_files -
                                                      max_connections)/
                                                      2/tc_size,
                                                     1),
                                              tc_instances),
                               uint32);
    /*
      If we have requested too much file handles than we bring
      max_connections in supported bounds. Still leave at least
      'min_connections' connections
    */
    SYSVAR_AUTOSIZE_IF_CHANGED(max_connections,
                               (ulong) MY_MAX(MY_MIN(files- extra_files-
                                                     min_tc_size*2*tc_instances,
                                                     max_connections),
                                              min_connections),
                               ulong);

    /*
      Decrease tc_size according to max_connections, but
      not below min_tc_size.  Outer MY_MIN() ensures that we
      never increase tc_size automatically (that could
      happen if max_connections is decreased above).
    */
    SYSVAR_AUTOSIZE_IF_CHANGED(tc_size,
                               (ulong) MY_MIN(MY_MAX((files - extra_files -
                                                      max_connections) / 2 / tc_instances,
                                                     min_tc_size),
                                              tc_size), ulong);
    DBUG_PRINT("warning",
               ("Current limits: max_open_files: %u  max_connections: %ld  table_cache: %ld",
                files, max_connections, tc_size));
    if (global_system_variables.log_warnings > 1 &&
        (max_connections < org_max_connections ||
         tc_size < org_tc_size))
      sql_print_warning("Changed limits: max_open_files: %u  max_connections: %lu (was %lu)  table_cache: %lu (was %lu)",
			files, max_connections, org_max_connections,
                        tc_size, org_tc_size);
  }
  /*
    Max_connections and tc_cache are now set.
    Now we can fix other variables depending on this variable.
  */

  /* Fix host_cache_size */
  if (IS_SYSVAR_AUTOSIZE(&host_cache_size))
  {
    /*
      The default value is 128.
      The autoset value is 128, plus 1 for a value of max_connections
      up to 500, plus 1 for every increment of 20 over 500 in the
      max_connections value, capped at 2000.
    */
    uint size= (HOST_CACHE_SIZE + MY_MIN(max_connections, 500) +
                MY_MAX(((long) max_connections)-500,0)/20);
    SYSVAR_AUTOSIZE(host_cache_size, size);
  }

  /* Fix back_log (back_log == 0 added for MySQL compatibility) */
  if (back_log == 0 || IS_SYSVAR_AUTOSIZE(&back_log))
  {
    /*
      The default value is 150.
      The autoset value is 50 + max_connections / 5 capped at 900
    */
    SYSVAR_AUTOSIZE(back_log, MY_MIN(900, (50 + max_connections / 5)));
  }

  unireg_init(opt_specialflag); /* Set up extern variabels */
  if (!(my_default_lc_messages=
        my_locale_by_name(lc_messages)))
  {
    sql_print_error("Unknown locale: '%s'", lc_messages);
    return 1;
  }

  if (init_errmessage())	/* Read error messages from file */
    return 1;
  global_system_variables.lc_messages= my_default_lc_messages;
  global_system_variables.errmsgs= my_default_lc_messages->errmsgs->errmsgs;
  init_client_errs();
  mysql_library_init(unused,unused,unused); /* for replication */
  lex_init();
  if (item_create_init())
    return 1;
  item_init();
  /*
    Process a comma-separated character set list and choose
    the first available character set. This is mostly for
    test purposes, to be able to start "mysqld" even if
    the requested character set is not available (see bug#18743).
  */
  myf utf8_flag= global_system_variables.old_behavior &
                 OLD_MODE_UTF8_IS_UTF8MB3 ? MY_UTF8_IS_UTF8MB3 : 0;
  for (;;)
  {
    char *next_character_set_name= strchr(default_character_set_name, ',');
    if (next_character_set_name)
      *next_character_set_name++= '\0';
    if (!(default_charset_info=
          get_charset_by_csname(default_character_set_name,
                                MY_CS_PRIMARY, MYF(utf8_flag | MY_WME))))
    {
      if (next_character_set_name)
      {
        default_character_set_name= next_character_set_name;
        default_collation_name= 0;          // Ignore collation
      }
      else
        return 1;                           // Eof of the list
    }
    else
      break;
  }

  if (default_collation_name)
  {
    CHARSET_INFO *default_collation;
    default_collation= get_charset_by_name(default_collation_name, MYF(utf8_flag));
    if (!default_collation)
    {
#ifdef WITH_PERFSCHEMA_STORAGE_ENGINE
      buffered_logs.print();
      buffered_logs.cleanup();
#endif
      sql_print_error(ER_DEFAULT(ER_UNKNOWN_COLLATION), default_collation_name);
      return 1;
    }
    if (!my_charset_same(default_charset_info, default_collation))
    {
      sql_print_error(ER_DEFAULT(ER_COLLATION_CHARSET_MISMATCH),
		      default_collation_name,
		      default_charset_info->cs_name.str);
      return 1;
    }
    default_charset_info= default_collation;
  }
  /* Set collactions that depends on the default collation */
  global_system_variables.collation_server= default_charset_info;
  global_system_variables.collation_database= default_charset_info;
  if (is_supported_parser_charset(default_charset_info))
  {
    global_system_variables.collation_connection= default_charset_info;
    global_system_variables.character_set_results= default_charset_info;
    global_system_variables.character_set_client= default_charset_info;
  }
  else
  {
    sql_print_warning("'%s' can not be used as client character set. "
                      "'%s' will be used as default client character set.",
                      default_charset_info->cs_name.str,
                      my_charset_latin1.cs_name.str);
    global_system_variables.collation_connection= &my_charset_latin1;
    global_system_variables.character_set_results= &my_charset_latin1;
    global_system_variables.character_set_client= &my_charset_latin1;
  }

  if (!(character_set_filesystem=
        get_charset_by_csname(character_set_filesystem_name,
                              MY_CS_PRIMARY, MYF(utf8_flag | MY_WME))))
    return 1;
  global_system_variables.character_set_filesystem= character_set_filesystem;

  if (!(my_default_lc_time_names=
        my_locale_by_name(lc_time_names_name)))
  {
    sql_print_error("Unknown locale: '%s'", lc_time_names_name);
    return 1;
  }
  global_system_variables.lc_time_names= my_default_lc_time_names;

  /* check log options and issue warnings if needed */
  if (opt_log && opt_logname && *opt_logname &&
      !(log_output_options & (LOG_FILE | LOG_NONE)))
    sql_print_warning("Although a path was specified for the "
                      "--log option, log tables are used. "
                      "To enable logging to files use the --log-output option.");

  if (global_system_variables.sql_log_slow && opt_slow_logname &&
      *opt_slow_logname &&
      !(log_output_options & (LOG_FILE | LOG_NONE)))
    sql_print_warning("Although a path was specified for the "
                      "--log-slow-queries option, log tables are used. "
                      "To enable logging to files use the --log-output=file option.");

  if (!opt_logname || !*opt_logname)
    make_default_log_name(&opt_logname, ".log", false);
  if (!opt_slow_logname || !*opt_slow_logname)
    make_default_log_name(&opt_slow_logname, "-slow.log", false);

#if defined(ENABLED_DEBUG_SYNC)
  /* Initialize the debug sync facility. See debug_sync.cc. */
  if (debug_sync_init())
    return 1; /* purecov: tested */
#endif /* defined(ENABLED_DEBUG_SYNC) */

#if (ENABLE_TEMP_POOL)
  if (use_temp_pool && my_bitmap_init(&temp_pool,0,1024))
    return 1;
#else
  use_temp_pool= 0;
#endif

  if (my_dboptions_cache_init())
    return 1;

  /*
    Ensure that lower_case_table_names is set on system where we have case
    insensitive names.  If this is not done the users MyISAM tables will
    get corrupted if accesses with names of different case.
  */
  DBUG_PRINT("info", ("lower_case_table_names: %d", lower_case_table_names));
  if(mysql_real_data_home_ptr == NULL || *mysql_real_data_home_ptr == 0)
    mysql_real_data_home_ptr= mysql_real_data_home;
  SYSVAR_AUTOSIZE(lower_case_file_system,
                  test_if_case_insensitive(mysql_real_data_home_ptr));
  if (!lower_case_table_names && lower_case_file_system == 1)
  {
    if (lower_case_table_names_used)
    {
      sql_print_error("The server option 'lower_case_table_names' is "
                      "configured to use case sensitive table names but the "
                      "data directory resides on a case-insensitive file system. "
                      "Please use a case sensitive file system for your data "
                      "directory or switch to a case-insensitive table name "
                      "mode.");
      return 1;
    }
    else
    {
      if (global_system_variables.log_warnings)
	sql_print_warning("Setting lower_case_table_names=2 because file "
                  "system for %s is case insensitive", mysql_real_data_home_ptr);
      SYSVAR_AUTOSIZE(lower_case_table_names, 2);
    }
  }
  else if (lower_case_table_names == 2 &&
           !(lower_case_file_system= (lower_case_file_system == 1)))
  {
    if (global_system_variables.log_warnings)
      sql_print_warning("lower_case_table_names was set to 2, even though your "
                        "the file system '%s' is case sensitive.  Now setting "
                        "lower_case_table_names to 0 to avoid future problems.",
			mysql_real_data_home_ptr);
    SYSVAR_AUTOSIZE(lower_case_table_names, 0);
  }
  else
  {
    lower_case_file_system= (lower_case_file_system == 1);
  }

  /* Reset table_alias_charset, now that lower_case_table_names is set. */
  table_alias_charset= (lower_case_table_names ?
			files_charset_info :
			&my_charset_bin);

  if (ignore_db_dirs_process_additions())
  {
    sql_print_error("An error occurred while storing ignore_db_dirs to a hash.");
    return 1;
  }


#ifdef WITH_WSREP
  /*
    We need to initialize auxiliary variables, that will be
    further keep the original values of auto-increment options
    as they set by the user. These variables used to restore
    user-defined values of the auto-increment options after
    setting of the wsrep_auto_increment_control to 'OFF'.
  */
  global_system_variables.saved_auto_increment_increment=
    global_system_variables.auto_increment_increment;
  global_system_variables.saved_auto_increment_offset=
    global_system_variables.auto_increment_offset;
#endif /* WITH_WSREP */

  return 0;
}


static int init_thread_environment()
{
  DBUG_ENTER("init_thread_environment");
  server_threads.init();
  mysql_mutex_init(key_LOCK_start_thread, &LOCK_start_thread, MY_MUTEX_INIT_FAST);
  mysql_mutex_init(key_LOCK_status, &LOCK_status, MY_MUTEX_INIT_FAST);
  mysql_mutex_init(key_LOCK_delayed_insert,
                   &LOCK_delayed_insert, MY_MUTEX_INIT_FAST);
  mysql_mutex_init(key_LOCK_delayed_status,
                   &LOCK_delayed_status, MY_MUTEX_INIT_FAST);
  mysql_mutex_init(key_LOCK_delayed_create,
                   &LOCK_delayed_create, MY_MUTEX_INIT_SLOW);
  mysql_mutex_init(key_LOCK_crypt, &LOCK_crypt, MY_MUTEX_INIT_FAST);
  mysql_mutex_init(key_LOCK_user_conn, &LOCK_user_conn, MY_MUTEX_INIT_FAST);
  mysql_mutex_init(key_LOCK_active_mi, &LOCK_active_mi, MY_MUTEX_INIT_FAST);
  mysql_mutex_init(key_LOCK_global_system_variables,
                   &LOCK_global_system_variables, MY_MUTEX_INIT_FAST);
  mysql_mutex_record_order(&LOCK_active_mi, &LOCK_global_system_variables);
  mysql_prlock_init(key_rwlock_LOCK_system_variables_hash,
                    &LOCK_system_variables_hash);
  mysql_mutex_init(key_LOCK_prepared_stmt_count,
                   &LOCK_prepared_stmt_count, MY_MUTEX_INIT_FAST);
  mysql_mutex_init(key_LOCK_error_messages,
                   &LOCK_error_messages, MY_MUTEX_INIT_FAST);
  mysql_mutex_init(key_LOCK_uuid_short_generator,
                   &LOCK_short_uuid_generator, MY_MUTEX_INIT_FAST);
  mysql_mutex_init(key_LOCK_thread_id,
                   &LOCK_thread_id, MY_MUTEX_INIT_FAST);
  mysql_mutex_init(key_LOCK_stats, &LOCK_stats, MY_MUTEX_INIT_FAST);
  mysql_mutex_init(key_LOCK_global_user_client_stats,
                   &LOCK_global_user_client_stats, MY_MUTEX_INIT_FAST);
  mysql_mutex_init(key_LOCK_global_table_stats,
                   &LOCK_global_table_stats, MY_MUTEX_INIT_FAST);
  mysql_mutex_init(key_LOCK_global_index_stats,
                   &LOCK_global_index_stats, MY_MUTEX_INIT_FAST);
  mysql_mutex_init(key_LOCK_prepare_ordered, &LOCK_prepare_ordered,
                   MY_MUTEX_INIT_SLOW);
  mysql_cond_init(key_COND_prepare_ordered, &COND_prepare_ordered, NULL);
  mysql_mutex_init(key_LOCK_after_binlog_sync, &LOCK_after_binlog_sync,
                   MY_MUTEX_INIT_SLOW);
  mysql_mutex_init(key_LOCK_commit_ordered, &LOCK_commit_ordered,
                   MY_MUTEX_INIT_SLOW);
  mysql_mutex_init(key_LOCK_backup_log, &LOCK_backup_log, MY_MUTEX_INIT_FAST);
  mysql_mutex_init(key_LOCK_temp_pool, &LOCK_temp_pool, MY_MUTEX_INIT_FAST);

#ifdef HAVE_OPENSSL
  mysql_mutex_init(key_LOCK_des_key_file,
                   &LOCK_des_key_file, MY_MUTEX_INIT_FAST);
#if defined(HAVE_OPENSSL10) && !defined(HAVE_WOLFSSL)
  openssl_stdlocks= (openssl_lock_t*) OPENSSL_malloc(CRYPTO_num_locks() *
                                                     sizeof(openssl_lock_t));
  for (int i= 0; i < CRYPTO_num_locks(); ++i)
    mysql_rwlock_init(key_rwlock_openssl, &openssl_stdlocks[i].lock);
  CRYPTO_set_dynlock_create_callback(openssl_dynlock_create);
  CRYPTO_set_dynlock_destroy_callback(openssl_dynlock_destroy);
  CRYPTO_set_dynlock_lock_callback(openssl_lock);
  CRYPTO_set_locking_callback(openssl_lock_function);
#endif /* HAVE_OPENSSL10 */
#endif /* HAVE_OPENSSL */
  mysql_rwlock_init(key_rwlock_LOCK_sys_init_connect, &LOCK_sys_init_connect);
  mysql_rwlock_init(key_rwlock_LOCK_sys_init_slave, &LOCK_sys_init_slave);
  mysql_rwlock_init(key_rwlock_LOCK_ssl_refresh, &LOCK_ssl_refresh);
  mysql_rwlock_init(key_rwlock_LOCK_grant, &LOCK_grant);
  mysql_rwlock_init(key_rwlock_LOCK_all_status_vars, &LOCK_all_status_vars);
  mysql_cond_init(key_COND_start_thread, &COND_start_thread, NULL);
#ifdef HAVE_REPLICATION
  mysql_mutex_init(key_LOCK_rpl_status, &LOCK_rpl_status, MY_MUTEX_INIT_FAST);
#endif
  mysql_mutex_init(key_LOCK_server_started,
                   &LOCK_server_started, MY_MUTEX_INIT_FAST);
  mysql_cond_init(key_COND_server_started, &COND_server_started, NULL);
  sp_cache_init();
#ifdef HAVE_EVENT_SCHEDULER
  Events::init_mutexes();
#endif
  init_show_explain_psi_keys();
  /* Parameter for threads created for connections */
  (void) pthread_attr_init(&connection_attrib);
  (void) pthread_attr_setdetachstate(&connection_attrib,
				     PTHREAD_CREATE_DETACHED);
  pthread_attr_setscope(&connection_attrib, PTHREAD_SCOPE_SYSTEM);

#ifdef HAVE_REPLICATION
  rpl_init_gtid_slave_state();
  rpl_init_gtid_waiting();
#endif

  DBUG_RETURN(0);
}


#if defined(HAVE_OPENSSL10) && !defined(HAVE_WOLFSSL)
static openssl_lock_t *openssl_dynlock_create(const char *file, int line)
{
  openssl_lock_t *lock= new openssl_lock_t;
  mysql_rwlock_init(key_rwlock_openssl, &lock->lock);
  return lock;
}


static void openssl_dynlock_destroy(openssl_lock_t *lock, const char *file,
				    int line)
{
  mysql_rwlock_destroy(&lock->lock);
  delete lock;
}


static void openssl_lock_function(int mode, int n, const char *file, int line)
{
  if (n < 0 || n > CRYPTO_num_locks())
  {
    /* Lock number out of bounds. */
    sql_print_error("Fatal: OpenSSL interface problem (n = %d)", n);
    abort();
  }
  openssl_lock(mode, &openssl_stdlocks[n], file, line);
}


static void openssl_lock(int mode, openssl_lock_t *lock, const char *file,
			 int line)
{
  int err;
  char const *what;

  switch (mode) {
  case CRYPTO_LOCK|CRYPTO_READ:
    what = "read lock";
    err= mysql_rwlock_rdlock(&lock->lock);
    break;
  case CRYPTO_LOCK|CRYPTO_WRITE:
    what = "write lock";
    err= mysql_rwlock_wrlock(&lock->lock);
    break;
  case CRYPTO_UNLOCK|CRYPTO_READ:
  case CRYPTO_UNLOCK|CRYPTO_WRITE:
    what = "unlock";
    err= mysql_rwlock_unlock(&lock->lock);
    break;
  default:
    /* Unknown locking mode. */
    sql_print_error("Fatal: OpenSSL interface problem (mode=0x%x)", mode);
    abort();
  }
  if (err)
  {
    sql_print_error("Fatal: can't %s OpenSSL lock", what);
    abort();
  }
}
#endif /* HAVE_OPENSSL10 */


struct SSL_ACCEPTOR_STATS
{
  long accept;
  long accept_good;
  long cache_size;
  long verify_mode;
  long verify_depth;
  long zero;
  const char *session_cache_mode;

  SSL_ACCEPTOR_STATS():
    accept(),accept_good(),cache_size(),verify_mode(),verify_depth(),zero(),
    session_cache_mode("NONE")
  {
  }

  void init()
  {
    DBUG_ASSERT(ssl_acceptor_fd !=0 && ssl_acceptor_fd->ssl_context != 0);
    SSL_CTX *ctx= ssl_acceptor_fd->ssl_context;
    accept= 0;
    accept_good= 0;
    verify_mode= SSL_CTX_get_verify_mode(ctx);
    verify_depth= SSL_CTX_get_verify_depth(ctx);
    cache_size= SSL_CTX_sess_get_cache_size(ctx);
    switch (SSL_CTX_get_session_cache_mode(ctx))
    {
    case SSL_SESS_CACHE_OFF:
      session_cache_mode= "OFF"; break;
    case SSL_SESS_CACHE_CLIENT:
      session_cache_mode= "CLIENT"; break;
    case SSL_SESS_CACHE_SERVER:
      session_cache_mode= "SERVER"; break;
    case SSL_SESS_CACHE_BOTH:
      session_cache_mode= "BOTH"; break;
    case SSL_SESS_CACHE_NO_AUTO_CLEAR:
      session_cache_mode= "NO_AUTO_CLEAR"; break;
    case SSL_SESS_CACHE_NO_INTERNAL_LOOKUP:
      session_cache_mode= "NO_INTERNAL_LOOKUP"; break;
    default:
      session_cache_mode= "Unknown"; break;
    }
  }
};

static SSL_ACCEPTOR_STATS ssl_acceptor_stats;
void ssl_acceptor_stats_update(int sslaccept_ret)
{
  statistic_increment(ssl_acceptor_stats.accept, &LOCK_status);
  if (!sslaccept_ret)
    statistic_increment(ssl_acceptor_stats.accept_good,&LOCK_status);
}

static void init_ssl()
{
/*
  Not need to check require_secure_transport on the Linux,
  because it always has Unix domain sockets that are secure:
*/
#ifdef _WIN32
  if (opt_require_secure_transport &&
      !opt_use_ssl &&
      !opt_enable_named_pipe &&
      !opt_bootstrap)
  {
    sql_print_error("Server is started with --require-secure-transport=ON "
                    "but no secure transport (SSL or PIPE) are configured.");
    unireg_abort(1);
  }
#endif
#if defined(HAVE_OPENSSL) && !defined(EMBEDDED_LIBRARY)
  if (opt_use_ssl)
  {
    enum enum_ssl_init_error error= SSL_INITERR_NOERROR;

    /* having ssl_acceptor_fd != 0 signals the use of SSL */
    ssl_acceptor_fd= new_VioSSLAcceptorFd(opt_ssl_key, opt_ssl_cert,
					  opt_ssl_ca, opt_ssl_capath,
					  opt_ssl_cipher, &error,
					  opt_ssl_crl, opt_ssl_crlpath,
					  tls_version);
    DBUG_PRINT("info",("ssl_acceptor_fd: %p", ssl_acceptor_fd));
    if (!ssl_acceptor_fd)
    {
      sql_print_warning("Failed to setup SSL");
      sql_print_warning("SSL error: %s", sslGetErrString(error));
      opt_use_ssl = 0;
      have_ssl= SHOW_OPTION_DISABLED;
    }
    else
      ssl_acceptor_stats.init();

    if (global_system_variables.log_warnings > 0)
    {
      ulong err;
      while ((err= ERR_get_error()))
      {
        char buf[256];
        ERR_error_string_n(err, buf, sizeof(buf));
        sql_print_warning("SSL error: %s",buf);
      }
    }
    else
      ERR_remove_state(0);
  }
  else
  {
    have_ssl= SHOW_OPTION_DISABLED;
  }
  if (des_key_file)
    load_des_key_file(des_key_file);
#endif /* HAVE_OPENSSL && ! EMBEDDED_LIBRARY */
}

/* Reinitialize SSL (FLUSH SSL) */
int reinit_ssl()
{
#if defined(HAVE_OPENSSL) && !defined(EMBEDDED_LIBRARY)
  if (!opt_use_ssl)
    return 0;

  enum enum_ssl_init_error error = SSL_INITERR_NOERROR;
  st_VioSSLFd *new_fd = new_VioSSLAcceptorFd(opt_ssl_key, opt_ssl_cert,
    opt_ssl_ca, opt_ssl_capath, opt_ssl_cipher, &error, opt_ssl_crl,
    opt_ssl_crlpath, tls_version);

  if (!new_fd)
  {
    my_printf_error(ER_UNKNOWN_ERROR, "Failed to refresh SSL, error: %s", MYF(0),
      sslGetErrString(error));
    ERR_clear_error();
    return 1;
  }
  mysql_rwlock_wrlock(&LOCK_ssl_refresh);
  free_vio_ssl_acceptor_fd(ssl_acceptor_fd);
  ssl_acceptor_fd= new_fd;
  ssl_acceptor_stats.init();
  mysql_rwlock_unlock(&LOCK_ssl_refresh);
#endif
  return 0;
}

static void end_ssl()
{
#ifdef HAVE_OPENSSL
#ifndef EMBEDDED_LIBRARY
  if (ssl_acceptor_fd)
  {
    free_vio_ssl_acceptor_fd(ssl_acceptor_fd);
    ssl_acceptor_fd= 0;
  }
#endif /* ! EMBEDDED_LIBRARY */
#endif /* HAVE_OPENSSL */
}

#ifdef _WIN32
/**
  Registers a file to be collected when Windows Error Reporting creates a crash 
  report.
*/
#include <werapi.h>
static void add_file_to_crash_report(char *file)
{
  wchar_t wfile[MAX_PATH+1]= {0};
  if (mbstowcs(wfile, file, MAX_PATH) != (size_t)-1)
  {
    WerRegisterFile(wfile, WerRegFileTypeOther, WER_FILE_ANONYMOUS_DATA);
  }
}
#endif

#define init_default_storage_engine(X,Y) \
  init_default_storage_engine_impl(#X, X, &global_system_variables.Y)

static int init_default_storage_engine_impl(const char *opt_name,
                                            char *engine_name, plugin_ref *res)
{
  if (!engine_name)
  {
    *res= 0;
    return 0;
  }

  LEX_CSTRING name= { engine_name, strlen(engine_name) };
  plugin_ref plugin;
  handlerton *hton;
  if ((plugin= ha_resolve_by_name(0, &name, false)))
    hton= plugin_hton(plugin);
  else
  {
    sql_print_error("Unknown/unsupported storage engine: %s", engine_name);
    return 1;
  }
  if (!ha_storage_engine_is_enabled(hton))
  {
    if (!opt_bootstrap)
    {
      sql_print_error("%s (%s) is not available", opt_name, engine_name);
      return 1;
    }
    DBUG_ASSERT(*res);
  }
  else
  {
    /*
      Need to unlock as global_system_variables.table_plugin
      was acquired during plugin_init()
    */
    mysql_mutex_lock(&LOCK_global_system_variables);
    if (*res)
      plugin_unlock(0, *res);
    *res= plugin;
    mysql_mutex_unlock(&LOCK_global_system_variables);
  }
  return 0;
}

static int
init_gtid_pos_auto_engines(void)
{
  plugin_ref *plugins;

  /*
    For the command-line option --gtid_pos_auto_engines, we allow (and ignore)
    engines that are unknown. This is convenient, since it allows to set
    default auto-create engines that might not be used by particular users.
    The option sets a list of storage engines that will have gtid position
    table auto-created for them if needed. And if the engine is not available,
    then it will certainly not be needed.
  */
  if (gtid_pos_auto_engines)
    plugins= resolve_engine_list(NULL, gtid_pos_auto_engines,
                                 strlen(gtid_pos_auto_engines), false, false);
  else
    plugins= resolve_engine_list(NULL, "", 0, false, false);
  if (!plugins)
    return 1;
  mysql_mutex_lock(&LOCK_global_system_variables);
  opt_gtid_pos_auto_plugins= plugins;
  mysql_mutex_unlock(&LOCK_global_system_variables);
  return 0;
}

#define MYSQL_COMPATIBILITY_OPTION(option) \
  { option, OPT_MYSQL_COMPATIBILITY, \
   0, 0, 0, 0, GET_STR, OPT_ARG, 0, 0, 0, 0, 0, 0 }

#define MYSQL_TO_BE_IMPLEMENTED_OPTION(option) \
  { option, OPT_MYSQL_TO_BE_IMPLEMENTED, \
   0, 0, 0, 0, GET_STR, OPT_ARG, 0, 0, 0, 0, 0, 0 }

#define MYSQL_SUGGEST_ANALOG_OPTION(option, str) \
  { option, OPT_MYSQL_COMPATIBILITY, \
   0, 0, 0, 0, GET_STR, OPT_ARG, 0, 0, 0, 0, 0, 0 }

#define MARIADB_REMOVED_OPTION(option) \
  { option, OPT_REMOVED_OPTION, \
   0, 0, 0, 0, GET_STR, OPT_ARG, 0, 0, 0, 0, 0, 0 }

static int init_server_components()
{
  DBUG_ENTER("init_server_components");
  /*
    We need to call each of these following functions to ensure that
    all things are initialized so that unireg_abort() doesn't fail
  */
  my_cpu_init();
  mdl_init();
  if (tdc_init() || hostname_cache_init())
    unireg_abort(1);

  query_cache_set_min_res_unit(query_cache_min_res_unit);
  query_cache_result_size_limit(query_cache_limit);
  /* if we set size of QC non zero in config then probably we want it ON */
  if (query_cache_size != 0 &&
      global_system_variables.query_cache_type == 0 &&
      !IS_SYSVAR_AUTOSIZE(&query_cache_size))
  {
    global_system_variables.query_cache_type= 1;
  }
  query_cache_init();
  DBUG_ASSERT(query_cache_size < ULONG_MAX);
  query_cache_resize((ulong)query_cache_size);
  my_rnd_init(&sql_rand,(ulong) server_start_time,(ulong) server_start_time/2);
  setup_fpu();
  init_thr_lock();
  backup_init();

  if (init_thr_timer(thread_scheduler->max_threads + extra_max_connections))
  {
    fprintf(stderr, "Can't initialize timers\n");
    unireg_abort(1);
  }

  my_uuid_init((ulong) (my_rnd(&sql_rand))*12345,12345);
  wt_init();

  /* Setup logs */

  setup_log_handling();

  /*
    Enable old-fashioned error log, except when the user has requested
    help information. Since the implementation of plugin server
    variables the help output is now written much later.
  */
#ifdef _WIN32
  if (opt_console)
   opt_error_log= false;
#endif

  if (opt_error_log && !opt_abort)
  {
    if (!log_error_file_ptr[0])
    {
      fn_format(log_error_file, pidfile_name, mysql_data_home, ".err",
                MY_REPLACE_EXT); /* replace '.<domain>' by '.err', bug#4997 */
      SYSVAR_AUTOSIZE(log_error_file_ptr, log_error_file);
    }
    else
    {
      fn_format(log_error_file, log_error_file_ptr, mysql_data_home, ".err",
                MY_UNPACK_FILENAME | MY_SAFE_PATH);
      log_error_file_ptr= log_error_file;
    }
    if (!log_error_file[0])
      opt_error_log= 0;                         // Too long file name
    else
    {
      my_bool res;
#ifndef EMBEDDED_LIBRARY
      res= reopen_fstreams(log_error_file, stdout, stderr);
#else
      res= reopen_fstreams(log_error_file, NULL, stderr);
#endif

      if (!res)
        setbuf(stderr, NULL);

#ifdef _WIN32
      /* Add error log to windows crash reporting. */
      add_file_to_crash_report(log_error_file);
#endif
    }
  }

  /* set up the hook before initializing plugins which may use it */
  error_handler_hook= my_message_sql;
  proc_info_hook= set_thd_stage_info;

#ifdef WITH_PERFSCHEMA_STORAGE_ENGINE
  /*
    Parsing the performance schema command line option may have reported
    warnings/information messages.
    Now that the logger is finally available, and redirected
    to the proper file when the --log--error option is used,
    print the buffered messages to the log.
  */
  buffered_logs.print();
  buffered_logs.cleanup();
#endif /* WITH_PERFSCHEMA_STORAGE_ENGINE */

#ifndef EMBEDDED_LIBRARY
  /*
    Now that the logger is available, redirect character set
    errors directly to the logger
    (instead of the buffered_logs used at the server startup time).
  */
  my_charset_error_reporter= charset_error_reporter;
#endif

  xid_cache_init();

  /* need to configure logging before initializing storage engines */
  if (!opt_bin_log_used && !WSREP_ON)
  {
    if (opt_log_slave_updates)
      sql_print_warning("You need to use --log-bin to make "
                        "--log-slave-updates work.");
    if (binlog_format_used)
      sql_print_warning("You need to use --log-bin to make "
                        "--binlog-format work.");
  }

  /* Check that we have not let the format to unspecified at this point */
  DBUG_ASSERT((uint)global_system_variables.binlog_format <=
              array_elements(binlog_format_names)-1);

#ifdef HAVE_REPLICATION
  if (opt_log_slave_updates && replicate_same_server_id)
  {
    if (opt_bin_log)
    {
      sql_print_error("using --replicate-same-server-id in conjunction with "
                      "--log-slave-updates is impossible, it would lead to "
                      "infinite loops in this server.");
      unireg_abort(1);
    }
    else
      sql_print_warning("using --replicate-same-server-id in conjunction with "
                        "--log-slave-updates would lead to infinite loops in "
                        "this server. However this will be ignored as the "
                        "--log-bin option is not defined.");
  }
#endif

  if (opt_bin_log)
  {
    /* Reports an error and aborts, if the --log-bin's path 
       is a directory.*/
    if (opt_bin_logname[0] && 
        opt_bin_logname[strlen(opt_bin_logname) - 1] == FN_LIBCHAR)
    {
      sql_print_error("Path '%s' is a directory name, please specify "
                      "a file name for --log-bin option", opt_bin_logname);
      unireg_abort(1);
    }

    /* Reports an error and aborts, if the --log-bin-index's path 
       is a directory.*/
    if (opt_binlog_index_name && 
        opt_binlog_index_name[strlen(opt_binlog_index_name) - 1] 
        == FN_LIBCHAR)
    {
      sql_print_error("Path '%s' is a directory name, please specify "
                      "a file name for --log-bin-index option",
                      opt_binlog_index_name);
      unireg_abort(1);
    }

    char buf[FN_REFLEN];
    const char *ln;
    ln= mysql_bin_log.generate_name(opt_bin_logname, "-bin", 1, buf);
    if (!opt_bin_logname[0] && !opt_binlog_index_name)
    {
      /*
        User didn't give us info to name the binlog index file.
        Picking `hostname`-bin.index like did in 4.x, causes replication to
        fail if the hostname is changed later. So, we would like to instead
        require a name. But as we don't want to break many existing setups, we
        only give warning, not error.
      */
      sql_print_warning("No argument was provided to --log-bin and "
                        "neither --log-basename or --log-bin-index where "
                        "used;  This may cause repliction to break when this "
                        "server acts as a master and has its hostname "
                        "changed! Please use '--log-basename=%s' or "
                        "'--log-bin=%s' to avoid this problem.",
                        opt_log_basename, ln);
    }
    if (ln == buf)
      opt_bin_logname= my_once_strdup(buf, MYF(MY_WME));
  }

  /*
    Since some wsrep threads (THDs) are create before plugins are
    initialized, LOCK_plugin mutex needs to be initialized here.
  */
  plugin_mutex_init();

  /*
    Wsrep initialization must happen at this point, because:
    - opt_bin_logname must be known when starting replication
      since SST may need it
    - SST may modify binlog index file, so it must be opened
      after SST has happened

    We also (unconditionally) initialize wsrep LOCKs and CONDs.
    It is because they are used while accessing wsrep system
    variables even when a wsrep provider is not loaded.
  */

  /* It's now safe to use thread specific memory */
  mysqld_server_initialized= 1;

#ifndef EMBEDDED_LIBRARY
  wsrep_thr_init();
#endif

#ifdef WITH_WSREP
  if (wsrep_init_server()) unireg_abort(1);

  if (WSREP_ON && !wsrep_recovery && !opt_abort)
  {
    if (opt_bootstrap) // bootsrap option given - disable wsrep functionality
    {
      wsrep_provider_init(WSREP_NONE);
      if (wsrep_init())
        unireg_abort(1);
    }
    else // full wsrep initialization
    {
      // add basedir/bin to PATH to resolve wsrep script names
      char* const tmp_path= (char*)my_alloca(strlen(mysql_home) +
                                             strlen("/bin") + 1);
      if (tmp_path)
      {
        strcpy(tmp_path, mysql_home);
        strcat(tmp_path, "/bin");
        wsrep_prepend_PATH(tmp_path);
      }
      else
      {
        WSREP_ERROR("Could not append %s/bin to PATH", mysql_home);
      }
      my_afree(tmp_path);

      if (wsrep_before_SE())
      {
        set_ports(); // this is also called in network_init() later but we need
                     // to know mysqld_port now - lp:1071882
        wsrep_init_startup(true);
      }
    }
  }
#endif /* WITH_WSREP */

  if (!opt_help && opt_bin_log)
  {
    if (mysql_bin_log.open_index_file(opt_binlog_index_name, opt_bin_logname,
                                      TRUE))
    {
      unireg_abort(1);
    }

    log_bin_basename=
      rpl_make_log_name(key_memory_MYSQL_BIN_LOG_basename,
                        opt_bin_logname, pidfile_name,
                        opt_bin_logname ? "" : "-bin");
    log_bin_index=
      rpl_make_log_name(key_memory_MYSQL_BIN_LOG_index,
                        opt_binlog_index_name, log_bin_basename, ".index");
    if (log_bin_basename == NULL || log_bin_index == NULL)
    {
      sql_print_error("Unable to create replication path names:"
                      " out of memory or path names too long"
                      " (path name exceeds " STRINGIFY_ARG(FN_REFLEN)
                      " or file name exceeds " STRINGIFY_ARG(FN_LEN) ").");
      unireg_abort(1);
    }
  }

#ifndef EMBEDDED_LIBRARY
  DBUG_PRINT("debug",
             ("opt_bin_logname: %s, opt_relay_logname: %s, pidfile_name: %s",
              opt_bin_logname, opt_relay_logname, pidfile_name));
  if (opt_relay_logname)
  {
    relay_log_basename=
      rpl_make_log_name(key_memory_MYSQL_RELAY_LOG_basename,
                        opt_relay_logname, pidfile_name,
                        opt_relay_logname ? "" : "-relay-bin");
    relay_log_index=
      rpl_make_log_name(key_memory_MYSQL_RELAY_LOG_index,
                        opt_relaylog_index_name, relay_log_basename, ".index");
    if (relay_log_basename == NULL || relay_log_index == NULL)
    {
      sql_print_error("Unable to create replication path names:"
                      " out of memory or path names too long"
                      " (path name exceeds " STRINGIFY_ARG(FN_REFLEN)
                      " or file name exceeds " STRINGIFY_ARG(FN_LEN) ").");
      unireg_abort(1);
    }
  }
#endif /* !EMBEDDED_LIBRARY */

  /* call ha_init_key_cache() on all key caches to init them */
  process_key_caches(&ha_init_key_cache, 0);

  init_global_table_stats();
  init_global_index_stats();
  init_update_queries();

  /* Allow storage engine to give real error messages */
  if (unlikely(ha_init_errors()))
    DBUG_RETURN(1);

  tc_log= 0; // ha_initialize_handlerton() needs that

  if (!opt_abort && ddl_log_initialize())
    unireg_abort(1);

  if (plugin_init(&remaining_argc, remaining_argv,
                  (opt_noacl ? PLUGIN_INIT_SKIP_PLUGIN_TABLE : 0) |
                  (opt_abort ? PLUGIN_INIT_SKIP_INITIALIZATION : 0)))
  {
    sql_print_error("Failed to initialize plugins.");
    unireg_abort(1);
  }
  plugins_are_initialized= TRUE;  /* Don't separate from init function */

#ifdef HAVE_REPLICATION
  /*
    Semisync is not required by other components, which justifies its
    initialization at this point when thread specific memory is also available.
  */
  if (repl_semisync_master.init_object() ||
      repl_semisync_slave.init_object())
  {
    sql_print_error("Could not initialize semisync.");
    unireg_abort(1);
  }
#endif

#ifndef EMBEDDED_LIBRARY
  if (session_tracker_init())
    return 1;
#endif //EMBEDDED_LIBRARY

  /* we do want to exit if there are any other unknown options */
  if (remaining_argc > 1)
  {
    int ho_error;
    struct my_option removed_opts[]=
    {
      /* The following options exist in 5.6 but not in 10.0 */
      MYSQL_COMPATIBILITY_OPTION("log-raw"),
      MYSQL_COMPATIBILITY_OPTION("log-bin-use-v1-row-events"),
      MYSQL_TO_BE_IMPLEMENTED_OPTION("default-authentication-plugin"),
      MYSQL_COMPATIBILITY_OPTION("binlog-max-flush-queue-time"),
      MYSQL_COMPATIBILITY_OPTION("master-info-repository"),
      MYSQL_COMPATIBILITY_OPTION("relay-log-info-repository"),
      MYSQL_SUGGEST_ANALOG_OPTION("binlog-rows-query-log-events", "--binlog-annotate-row-events"),
      MYSQL_COMPATIBILITY_OPTION("binlog-order-commits"),
      MYSQL_TO_BE_IMPLEMENTED_OPTION("log-throttle-queries-not-using-indexes"),
      MYSQL_TO_BE_IMPLEMENTED_OPTION("end-markers-in-json"),
      MYSQL_TO_BE_IMPLEMENTED_OPTION("optimizer-trace-features"),     // OPTIMIZER_TRACE
      MYSQL_TO_BE_IMPLEMENTED_OPTION("optimizer-trace-offset"),       // OPTIMIZER_TRACE
      MYSQL_TO_BE_IMPLEMENTED_OPTION("optimizer-trace-limit"),        // OPTIMIZER_TRACE
      MYSQL_COMPATIBILITY_OPTION("server-id-bits"),
      MYSQL_TO_BE_IMPLEMENTED_OPTION("slave-rows-search-algorithms"), // HAVE_REPLICATION
      MYSQL_TO_BE_IMPLEMENTED_OPTION("slave-allow-batching"),         // HAVE_REPLICATION
      MYSQL_COMPATIBILITY_OPTION("slave-checkpoint-period"),      // HAVE_REPLICATION
      MYSQL_COMPATIBILITY_OPTION("slave-checkpoint-group"),       // HAVE_REPLICATION
      MYSQL_SUGGEST_ANALOG_OPTION("slave-pending-jobs-size-max", "--slave-parallel-max-queued"),  // HAVE_REPLICATION
      MYSQL_TO_BE_IMPLEMENTED_OPTION("sha256-password-private-key-path"), // HAVE_OPENSSL
      MYSQL_TO_BE_IMPLEMENTED_OPTION("sha256-password-public-key-path"),  // HAVE_OPENSSL

      /* The following options exist in 5.5 and 5.6 but not in 10.0 */
      MYSQL_SUGGEST_ANALOG_OPTION("abort-slave-event-count", "--debug-abort-slave-event-count"),
      MYSQL_SUGGEST_ANALOG_OPTION("disconnect-slave-event-count", "--debug-disconnect-slave-event-count"),
      MYSQL_SUGGEST_ANALOG_OPTION("exit-info", "--debug-exit-info"),
      MYSQL_SUGGEST_ANALOG_OPTION("max-binlog-dump-events", "--debug-max-binlog-dump-events"),
      MYSQL_SUGGEST_ANALOG_OPTION("sporadic-binlog-dump-fail", "--debug-sporadic-binlog-dump-fail"),
      MYSQL_COMPATIBILITY_OPTION("new"),
      MYSQL_COMPATIBILITY_OPTION("show_compatibility_56"),

      /* The following options were removed in 10.6 */
      MARIADB_REMOVED_OPTION("innodb-force-load-corrupted"),

      /* The following options were removed in 10.5 */
#if defined(__linux__)
      MARIADB_REMOVED_OPTION("super-large-pages"),
#endif
      MARIADB_REMOVED_OPTION("innodb-idle-flush-pct"),
      MARIADB_REMOVED_OPTION("innodb-locks-unsafe-for-binlog"),
      MARIADB_REMOVED_OPTION("innodb-rollback-segments"),
      MARIADB_REMOVED_OPTION("innodb-stats-sample-pages"),
      MARIADB_REMOVED_OPTION("max-long-data-size"),
      MARIADB_REMOVED_OPTION("multi-range-count"),
      MARIADB_REMOVED_OPTION("skip-bdb"),
      MARIADB_REMOVED_OPTION("thread-concurrency"),
      MARIADB_REMOVED_OPTION("timed-mutexes"),

      /* The following options were added after 5.6.10 */
      MYSQL_TO_BE_IMPLEMENTED_OPTION("rpl-stop-slave-timeout"),
      MYSQL_TO_BE_IMPLEMENTED_OPTION("validate-user-plugins"), // NO_EMBEDDED_ACCESS_CHECKS

      /* The following options were deprecated in 10.5 or earlier */
      MARIADB_REMOVED_OPTION("innodb-adaptive-max-sleep-delay"),
      MARIADB_REMOVED_OPTION("innodb-background-scrub-data-check-interval"),
      MARIADB_REMOVED_OPTION("innodb-background-scrub-data-compressed"),
      MARIADB_REMOVED_OPTION("innodb-background-scrub-data-interval"),
      MARIADB_REMOVED_OPTION("innodb-background-scrub-data-uncompressed"),
      MARIADB_REMOVED_OPTION("innodb-buffer-pool-instances"),
      MARIADB_REMOVED_OPTION("innodb-commit-concurrency"),
      MARIADB_REMOVED_OPTION("innodb-concurrency-tickets"),
      MARIADB_REMOVED_OPTION("innodb-file-format"),
      MARIADB_REMOVED_OPTION("innodb-large-prefix"),
      MARIADB_REMOVED_OPTION("innodb-lock-schedule-algorithm"),
      MARIADB_REMOVED_OPTION("innodb-log-checksums"),
      MARIADB_REMOVED_OPTION("innodb-log-compressed-pages"),
      MARIADB_REMOVED_OPTION("innodb-log-files-in-group"),
      MARIADB_REMOVED_OPTION("innodb-log-optimize-ddl"),
      MARIADB_REMOVED_OPTION("innodb-log-write-ahead-size"),
      MARIADB_REMOVED_OPTION("innodb-page-cleaners"),
      MARIADB_REMOVED_OPTION("innodb-replication-delay"),
      MARIADB_REMOVED_OPTION("innodb-scrub-log"),
      MARIADB_REMOVED_OPTION("innodb-scrub-log-speed"),
      MARIADB_REMOVED_OPTION("innodb-sync-array-size"),
      MARIADB_REMOVED_OPTION("innodb-thread-concurrency"),
      MARIADB_REMOVED_OPTION("innodb-thread-sleep-delay"),
      MARIADB_REMOVED_OPTION("innodb-undo-logs"),
      {0, 0, 0, 0, 0, 0, GET_NO_ARG, NO_ARG, 0, 0, 0, 0, 0, 0}
    };
    /*
      We need to eat any 'loose' arguments first before we conclude
      that there are unprocessed options.
    */
    my_getopt_skip_unknown= 0;
#ifdef WITH_WSREP
    if (wsrep_recovery)
      my_getopt_skip_unknown= TRUE;
#endif

    if ((ho_error= handle_options(&remaining_argc, &remaining_argv, removed_opts,
                                  mysqld_get_one_option)))
      unireg_abort(ho_error);
    /* Add back the program name handle_options removes */
    remaining_argc++;
    remaining_argv--;
    my_getopt_skip_unknown= TRUE;

#ifdef WITH_WSREP
    if (!wsrep_recovery)
    {
#endif
      if (remaining_argc > 1)
      {
        fprintf(stderr, "%s: Too many arguments (first extra is '%s').\n",
                my_progname, remaining_argv[1]);
        unireg_abort(1);
      }
#ifdef WITH_WSREP
    }
#endif
  }

  if (opt_abort)
    unireg_abort(0);

  if (init_io_cache_encryption())
    unireg_abort(1);

  /* if the errmsg.sys is not loaded, terminate to maintain behaviour */
  if (!DEFAULT_ERRMSGS[0][0])
    unireg_abort(1);  

  /* We have to initialize the storage engines before CSV logging */
  if (ha_init())
  {
    sql_print_error("Can't init databases");
    unireg_abort(1);
  }

  if (opt_bootstrap)
    log_output_options= LOG_FILE;
  else
    logger.init_log_tables();

  if (log_output_options & LOG_NONE)
  {
    /*
      Issue a warning if there were specified additional options to the
      log-output along with NONE. Probably this wasn't what user wanted.
    */
    if ((log_output_options & LOG_NONE) && (log_output_options & ~LOG_NONE))
      sql_print_warning("There were other values specified to "
                        "log-output besides NONE. Disabling slow "
                        "and general logs anyway.");
    logger.set_handlers(LOG_NONE, LOG_NONE);
  }
  else
  {
    /* fall back to the log files if tables are not present */
    LEX_CSTRING csv_name={STRING_WITH_LEN("csv")};
    if (!plugin_is_ready(&csv_name, MYSQL_STORAGE_ENGINE_PLUGIN))
    {
      /* purecov: begin inspected */
      sql_print_error("CSV engine is not present, falling back to the "
                      "log files");
      SYSVAR_AUTOSIZE(log_output_options, 
                      (log_output_options & ~LOG_TABLE) | LOG_FILE);
      /* purecov: end */
    }

    logger.set_handlers(global_system_variables.sql_log_slow ?
                        log_output_options:LOG_NONE,
                        opt_log ? log_output_options:LOG_NONE);
  }

  if (init_default_storage_engine(default_storage_engine, table_plugin))
    unireg_abort(1);

  if (default_tmp_storage_engine && !*default_tmp_storage_engine)
    default_tmp_storage_engine= NULL;

  if (enforced_storage_engine && !*enforced_storage_engine)
    enforced_storage_engine= NULL;

  if (init_default_storage_engine(default_tmp_storage_engine, tmp_table_plugin))
    unireg_abort(1);

  if (init_default_storage_engine(enforced_storage_engine, enforced_table_plugin))
    unireg_abort(1);

  if (init_gtid_pos_auto_engines())
    unireg_abort(1);

#ifdef USE_ARIA_FOR_TMP_TABLES
  if (!ha_storage_engine_is_enabled(maria_hton) && !opt_bootstrap)
  {
    sql_print_error("Aria engine is not enabled or did not start. The Aria engine must be enabled to continue as server was configured with --with-aria-tmp-tables");
    unireg_abort(1);
  }
#endif

#ifdef WITH_WSREP
  /*
    Now is the right time to initialize members of wsrep startup threads
    that rely on plugins and other related global system variables to be
    initialized. This initialization was not possible before, as plugins
    (and thus some global system variables) are initialized after wsrep
    startup threads are created.
    Note: This only needs to be done for rsync and mariabackup based SST
    methods.
  */
  if (wsrep_before_SE())
    wsrep_plugins_post_init();

  if (WSREP_ON && !opt_bin_log)
  {
    wsrep_emulate_bin_log= 1;
  }
#endif

  tc_log= get_tc_log_implementation();

  if (tc_log->open(opt_bin_log ? opt_bin_logname : opt_tc_log_file))
  {
    sql_print_error("Can't init tc log");
    unireg_abort(1);
  }

  if (ha_recover(0))
    unireg_abort(1);

#ifndef EMBEDDED_LIBRARY
  start_handle_manager();
#endif
  if (opt_bin_log)
  {
    int error;
    mysql_mutex_t *log_lock= mysql_bin_log.get_log_lock();
    mysql_mutex_lock(log_lock);
    error= mysql_bin_log.open(opt_bin_logname, 0, 0,
                              WRITE_CACHE, max_binlog_size, 0, TRUE);
    mysql_mutex_unlock(log_lock);
    if (unlikely(error))
      unireg_abort(1);
  }

#ifdef HAVE_REPLICATION
  if (opt_bin_log)
  {
    if (binlog_expire_logs_seconds)
    {
      time_t purge_time= server_start_time - binlog_expire_logs_seconds;
      if (purge_time >= 0)
        mysql_bin_log.purge_logs_before_date(purge_time);
    }
  }
  else
  {
    if (binlog_expire_logs_seconds)
      sql_print_warning("You need to use --log-bin to make --expire-logs-days "
                        "or --binlog-expire-logs-seconds work.");
  }
#endif

  if (ddl_log_execute_recovery() > 0)
    unireg_abort(1);
  ha_signal_ddl_recovery_done();

  if (opt_myisam_log)
    (void) mi_log(1);

#if defined(HAVE_MLOCKALL) && defined(MCL_CURRENT) && !defined(EMBEDDED_LIBRARY)
  if (locked_in_memory)
  {
    int error;
    if (user_info)
    {
      DBUG_ASSERT(!getuid());
      if (setreuid((uid_t) -1, 0) == -1)
      {
        sql_perror("setreuid");
        unireg_abort(1);
      }
      error= mlockall(MCL_CURRENT);
      set_user(mysqld_user, user_info);
    }
    else
      error= mlockall(MCL_CURRENT);

    if (unlikely(error))
    {
      if (global_system_variables.log_warnings)
	sql_print_warning("Failed to lock memory. Errno: %d\n",errno);
      locked_in_memory= 0;
    }
  }
#else
  locked_in_memory= 0;
#endif

  ft_init_stopwords();

  init_max_user_conn();
  init_global_user_stats();
  init_global_client_stats();
  if (!opt_bootstrap)
    servers_init(0);
  init_status_vars();
  Item_false= new (&read_only_root) Item_bool_static("FALSE", 0);
  Item_true=  new (&read_only_root) Item_bool_static("TRUE", 1);
  DBUG_ASSERT(Item_false);

  DBUG_RETURN(0);
}


#ifndef EMBEDDED_LIBRARY

#ifndef DBUG_OFF
/*
  Debugging helper function to keep the locale database
  (see sql_locale.cc) and max_month_name_length and
  max_day_name_length variable values in consistent state.
*/
static void test_lc_time_sz()
{
  DBUG_ENTER("test_lc_time_sz");
  for (MY_LOCALE **loc= my_locales; *loc; loc++)
  {
    size_t max_month_len= 0;
    size_t max_day_len= 0;
    for (const char **month= (*loc)->month_names->type_names; *month; month++)
    {
      set_if_bigger(max_month_len,
                    my_numchars_mb(&my_charset_utf8mb3_general_ci,
                                   *month, *month + strlen(*month)));
    }
    for (const char **day= (*loc)->day_names->type_names; *day; day++)
    {
      set_if_bigger(max_day_len,
                    my_numchars_mb(&my_charset_utf8mb3_general_ci,
                                   *day, *day + strlen(*day)));
    }
    if ((*loc)->max_month_name_length != max_month_len ||
        (*loc)->max_day_name_length != max_day_len)
    {
      DBUG_PRINT("Wrong max day name(or month name) length for locale:",
                 ("%s", (*loc)->name));
      DBUG_ASSERT(0);
    }
  }
  DBUG_VOID_RETURN;
}
#endif//DBUG_OFF


int mysqld_main(int argc, char **argv)
{
#ifndef _WIN32
  /* We can't close stdin just now, because it may be booststrap mode. */
  bool please_close_stdin= fcntl(STDIN_FILENO, F_GETFD) >= 0;
#endif

  /*
    Perform basic thread library and malloc initialization,
    to be able to read defaults files and parse options.
  */
  my_progname= argv[0];
  sf_leaking_memory= 1; // no safemalloc memory leak reports if we exit early
  mysqld_server_started= mysqld_server_initialized= 0;

  if (init_early_variables())
    exit(1);

#ifdef WITH_PERFSCHEMA_STORAGE_ENGINE
  pre_initialize_performance_schema();
#endif /*WITH_PERFSCHEMA_STORAGE_ENGINE */

  if (my_init())                 // init my_sys library & pthreads
  {
    fprintf(stderr, "my_init() failed.");
    return 1;
  }

  orig_argc= argc;
  orig_argv= argv;
  my_defaults_mark_files= TRUE;
  load_defaults_or_exit(MYSQL_CONFIG_NAME, load_default_groups, &argc, &argv);
  defaults_argc= argc;
  defaults_argv= argv;
  remaining_argc= argc;
  remaining_argv= argv;

  /* Must be initialized early for comparison of options name */
  system_charset_info= &my_charset_utf8mb3_general_ci;

  sys_var_init();

#ifdef WITH_PERFSCHEMA_STORAGE_ENGINE
  /*
    Initialize the array of performance schema instrument configurations.
  */
  init_pfs_instrument_array();

  /*
    Logs generated while parsing the command line
    options are buffered and printed later.
  */
  buffered_logs.init();
  my_getopt_error_reporter= buffered_option_error_reporter;
  my_charset_error_reporter= buffered_option_error_reporter;

  pfs_param.m_pfs_instrument= const_cast<char*>("");
#endif /* WITH_PERFSCHEMA_STORAGE_ENGINE */
  my_timer_init(&sys_timer_info);

  int ho_error __attribute__((unused))= handle_early_options();

  /* fix tdc_size */
  if (IS_SYSVAR_AUTOSIZE(&tdc_size))
  {
    SYSVAR_AUTOSIZE(tdc_size, MY_MIN(400 + tdc_size / 2, 2000));
  }

#ifdef WITH_PERFSCHEMA_STORAGE_ENGINE
  if (ho_error == 0)
  {
    if (pfs_param.m_enabled  && !opt_help && !opt_bootstrap)
    {
      /* Add sizing hints from the server sizing parameters. */
      pfs_param.m_hints.m_table_definition_cache= tdc_size;
      pfs_param.m_hints.m_table_open_cache= tc_size;
      pfs_param.m_hints.m_max_connections= max_connections;
      pfs_param.m_hints.m_open_files_limit= open_files_limit;
      PSI_hook= initialize_performance_schema(&pfs_param);
      if (PSI_hook == NULL)
      {
        pfs_param.m_enabled= false;
        buffered_logs.buffer(WARNING_LEVEL,
                             "Performance schema disabled (reason: init failed).");
      }
    }
  }
#else
  /*
    Other provider of the instrumentation interface should
    initialize PSI_hook here:
    - HAVE_PSI_INTERFACE is for the instrumentation interface
    - WITH_PERFSCHEMA_STORAGE_ENGINE is for one implementation
      of the interface,
    but there could be alternate implementations, which is why
    these two defines are kept separate.
  */
#endif /* WITH_PERFSCHEMA_STORAGE_ENGINE */

#ifdef HAVE_PSI_INTERFACE
  /*
    Obtain the current performance schema instrumentation interface,
    if available.
  */
  if (PSI_hook)
  {
    PSI *psi_server= (PSI*) PSI_hook->get_interface(PSI_CURRENT_VERSION);
    if (likely(psi_server != NULL))
    {
      set_psi_server(psi_server);

      /*
        Now that we have parsed the command line arguments, and have
        initialized the performance schema itself, the next step is to
        register all the server instruments.
      */
      init_server_psi_keys();
      /* Instrument the main thread */
      PSI_thread *psi= PSI_CALL_new_thread(key_thread_main, NULL, 0);
      PSI_CALL_set_thread_os_id(psi);
      PSI_CALL_set_thread(psi);

      /*
        Now that some instrumentation is in place,
        recreate objects which were initialised early,
        so that they are instrumented as well.
      */
      my_thread_global_reinit();
    }
  }
#endif /* HAVE_PSI_INTERFACE */

  mysql_mutex_init(key_LOCK_error_log, &LOCK_error_log, MY_MUTEX_INIT_FAST);

  /* Initialize audit interface globals. Audit plugins are inited later. */
  mysql_audit_initialize();

  /*
    Perform basic logger initialization logger. Should be called after
    MY_INIT, as it initializes mutexes. Log tables are inited later.
  */
  logger.init_base();

#ifdef WITH_PERFSCHEMA_STORAGE_ENGINE
  if (ho_error)
  {
    /*
      Parsing command line option failed,
      Since we don't have a workable remaining_argc/remaining_argv
      to continue the server initialization, this is as far as this
      code can go.
      This is the best effort to log meaningful messages:
      - messages will be printed to stderr, which is not redirected yet,
      - messages will be printed in the NT event log, for windows.
    */
    buffered_logs.print();
    buffered_logs.cleanup();
    /*
      Not enough initializations for unireg_abort()
      Using exit() for windows.
    */
    exit (ho_error);
  }
#endif /* WITH_PERFSCHEMA_STORAGE_ENGINE */

#ifdef _CUSTOMSTARTUPCONFIG_
  if (_cust_check_startup())
  {
    / * _cust_check_startup will report startup failure error * /
    exit(1);
  }
#endif

  if (init_common_variables())
    unireg_abort(1);				// Will do exit

  init_signals();

  ulonglong new_thread_stack_size;
  new_thread_stack_size= my_setstacksize(&connection_attrib,
                                         (size_t)my_thread_stack_size);
  if (new_thread_stack_size != my_thread_stack_size)
  {
    if ((new_thread_stack_size < my_thread_stack_size) &&
        global_system_variables.log_warnings)
      sql_print_warning("Asked for %llu thread stack, but got %llu",
                        my_thread_stack_size, new_thread_stack_size);
    SYSVAR_AUTOSIZE(my_thread_stack_size, new_thread_stack_size);
  }

  (void) thr_setconcurrency(concurrency);	// 10 by default

  select_thread=pthread_self();
  select_thread_in_use=1;

#ifdef HAVE_LIBWRAP
  libwrapName= my_progname+dirname_length(my_progname);
  openlog(libwrapName, LOG_PID, LOG_AUTH);
#endif

#ifndef DBUG_OFF
  test_lc_time_sz();
  srand((uint) time(NULL)); 
#endif

  /*
    We have enough space for fiddling with the argv, continue
  */
  check_data_home(mysql_real_data_home);
  if (my_setwd(mysql_real_data_home, opt_abort ? 0 : MYF(MY_WME)) && !opt_abort)
    unireg_abort(1);				/* purecov: inspected */

  /* Atomic write initialization must be done as root */
  my_init_atomic_write();

  if ((user_info= check_user(mysqld_user)))
  {
#if defined(HAVE_MLOCKALL) && defined(MCL_CURRENT)
    if (locked_in_memory) // getuid() == 0 here
      set_effective_user(user_info);
    else
#endif
      set_user(mysqld_user, user_info);
  }

#ifdef WITH_WSREP
  wsrep_set_wsrep_on(nullptr);
  if (WSREP_ON && wsrep_check_opts()) unireg_abort(1);
#endif

#ifdef _WIN32
  /* 
   The subsequent calls may take a long time : e.g. innodb log read.
   Thus set the long running service control manager timeout
  */
  my_report_svc_status(SERVICE_START_PENDING, NO_ERROR, slow_start_timeout);
#endif

  if (init_server_components())
    unireg_abort(1);

  init_ssl();
  network_init();

#ifdef WITH_WSREP
  // Recover and exit.
  if (wsrep_recovery)
  {
    select_thread_in_use= 0;
    if (WSREP_ON)
      wsrep_recover();
    else
      sql_print_information("WSREP: disabled, skipping position recovery");
    unireg_abort(0);
  }
#endif

  /*
    init signals & alarm
    After this we can't quit by a simple unireg_abort
  */
  start_signal_handler();				// Creates pidfile

  if (mysql_rm_tmp_tables() || acl_init(opt_noacl) ||
      my_tz_init((THD *)0, default_tz_name, opt_bootstrap))
    unireg_abort(1);

  if (!opt_noacl)
    (void) grant_init();

  udf_init();

  if (opt_bootstrap) /* If running with bootstrap, do not start replication. */
    opt_skip_slave_start= 1;

  binlog_unsafe_map_init();

#ifdef WITH_PERFSCHEMA_STORAGE_ENGINE
  initialize_performance_schema_acl(opt_bootstrap);
#endif

  initialize_information_schema_acl();

  /*
    Change EVENTS_ORIGINAL to EVENTS_OFF (the default value) as there is no
    point in using ORIGINAL during startup
  */
  if (Events::opt_event_scheduler == Events::EVENTS_ORIGINAL)
    Events::opt_event_scheduler= Events::EVENTS_OFF;

  Events::set_original_state(Events::opt_event_scheduler);
  if (Events::init((THD*) 0, opt_noacl || opt_bootstrap))
    unireg_abort(1);

#ifdef WITH_WSREP
  if (WSREP_ON)
  {
    if (opt_bootstrap)
    {
      /*! bootstrap wsrep init was taken care of above */
    }
    else
    {
      wsrep_init_globals();
      if (!wsrep_before_SE())
      {
        wsrep_init_startup(false);
      }
      wsrep_new_cluster= false;
      if (wsrep_cluster_address_exists())
      {
        WSREP_DEBUG("Startup creating %ld applier threads running %lu",
                wsrep_slave_threads - 1, wsrep_running_applier_threads);
        wsrep_create_appliers(wsrep_slave_threads - 1);
      }
    }
  }
#endif /* WITH_WSREP */

  /* Protect read_only_root against writes */
  protect_root(&read_only_root, PROT_READ);

  if (opt_bootstrap)
  {
    select_thread_in_use= 0;                    // Allow 'kill' to work
    int bootstrap_error= bootstrap(mysql_stdin);
    if (!abort_loop)
      unireg_abort(bootstrap_error);
    else
    {
      sleep(2);                                 // Wait for kill
      exit(0);
    }
  }

  /* Copy default global rpl_filter to global_rpl_filter */
  copy_filter_setting(global_rpl_filter, get_or_create_rpl_filter("", 0));

  /*
    init_slave() must be called after the thread keys are created.
    Some parts of the code (e.g. SHOW STATUS LIKE 'slave_running' and other
    places) assume that active_mi != 0, so let's fail if it's 0 (out of
    memory); a message has already been printed.
  */
  if (init_slave() && !active_mi)
  {
    unireg_abort(1);
  }

  if (opt_init_file && *opt_init_file)
  {
    if (read_init_file(opt_init_file))
      unireg_abort(1);
  }

  disable_log_notes= 0; /* Startup done, now we can give notes again */

  if (IS_SYSVAR_AUTOSIZE(&server_version_ptr))
    sql_print_information(ER_DEFAULT(ER_STARTUP), my_progname, server_version,
                          (systemd_sock_activation ? "Systemd socket activated ports" :
                            (unix_sock_is_online ? mysqld_unix_port : (char*) "")),
                          mysqld_port, MYSQL_COMPILATION_COMMENT);
  else
  {
    char real_server_version[2 * SERVER_VERSION_LENGTH + 10];

    set_server_version(real_server_version, sizeof(real_server_version));
    strcat(real_server_version, "' as '");
    strcat(real_server_version, server_version);

    sql_print_information(ER_DEFAULT(ER_STARTUP), my_progname,
                          real_server_version,
                          (systemd_sock_activation ? "Systemd socket activated ports" :
                            (unix_sock_is_online ? mysqld_unix_port : (char*) "")),
                          mysqld_port, MYSQL_COMPILATION_COMMENT);
  }

#ifndef _WIN32
  // try to keep fd=0 busy
  if (please_close_stdin && !freopen("/dev/null", "r", stdin))
  {
    // fall back on failure
    fclose(stdin);
  }
#endif


  /* Signal threads waiting for server to be started */
  mysql_mutex_lock(&LOCK_server_started);
  mysqld_server_started= 1;
  mysql_cond_broadcast(&COND_server_started);
  mysql_mutex_unlock(&LOCK_server_started);

  (void)MYSQL_SET_STAGE(0 ,__FILE__, __LINE__);

  /* Memory used when everything is setup */
  start_memory_used= global_status_var.global_memory_used;

#ifdef _WIN32
  handle_connections_win();
#else
  handle_connections_sockets();

  mysql_mutex_lock(&LOCK_start_thread);
  select_thread_in_use=0;
  mysql_cond_broadcast(&COND_start_thread);
  mysql_mutex_unlock(&LOCK_start_thread);
#endif /* _WIN32 */

  /* Shutdown requested */
  char *user= shutdown_user.load(std::memory_order_relaxed);
  sql_print_information(ER_DEFAULT(ER_NORMAL_SHUTDOWN), my_progname,
                        user ? user : "unknown");
  if (user)
    my_free(user);

#ifdef WITH_WSREP
  /* Stop wsrep threads in case they are running. */
  if (wsrep_running_threads > 0)
  {
    wsrep_shutdown_replication();
  }
  /* Release threads if they are waiting in WSREP_SYNC_WAIT_UPTO_GTID */
  wsrep_gtid_server.signal_waiters(0, true);
#endif

  close_connections();
  ha_pre_shutdown();
  clean_up(1);
  sd_notify(0, "STATUS=MariaDB server is down");

  /* (void) pthread_attr_destroy(&connection_attrib); */

  DBUG_PRINT("quit",("Exiting main thread"));

  /*
    Disable the main thread instrumentation,
    to avoid recording events during the shutdown.
  */
  PSI_CALL_delete_current_thread();

#if (defined(HAVE_OPENSSL) && !defined(EMBEDDED_LIBRARY))
  ERR_remove_state(0);
#endif
  mysqld_exit(0);
  return 0;
}

#endif /* !EMBEDDED_LIBRARY */


static bool read_init_file(char *file_name)
{
  MYSQL_FILE *file;
  DBUG_ENTER("read_init_file");
  DBUG_PRINT("enter",("name: %s",file_name));
  if (!(file= mysql_file_fopen(key_file_init, file_name,
                               O_RDONLY, MYF(MY_WME))))
    DBUG_RETURN(TRUE);
  bootstrap(file);
  mysql_file_fclose(file, MYF(MY_WME));
  DBUG_RETURN(FALSE);
}


/**
  Increment number of created threads
*/
void inc_thread_created(void)
{
  statistic_increment(thread_created, &LOCK_status);
}

#ifndef EMBEDDED_LIBRARY

/*
   Simple scheduler that use the main thread to handle the request

   NOTES
     This is only used for debugging, when starting mysqld with
     --thread-handling=no-threads or --one-thread
*/

void handle_connection_in_main_thread(CONNECT *connect)
{
  do_handle_one_connection(connect, false);
}


/*
  Scheduler that uses one thread per connection
*/

void create_thread_to_handle_connection(CONNECT *connect)
{
  DBUG_ENTER("create_thread_to_handle_connection");

  if (thread_cache.enqueue(connect))
    DBUG_VOID_RETURN;

  /* Create new thread to handle connection */
  inc_thread_created();
  DBUG_PRINT("info",(("creating thread %lu"), (ulong) connect->thread_id));
  connect->prior_thr_create_utime= microsecond_interval_timer();

  pthread_t tmp;
  if (auto error= mysql_thread_create(key_thread_one_connection,
                                      &tmp, &connection_attrib,
                                      handle_one_connection, (void*) connect))
  {
    char error_message_buff[MYSQL_ERRMSG_SIZE];
    /* purecov: begin inspected */
    DBUG_PRINT("error", ("Can't create thread to handle request (error %d)",
                error));
    my_snprintf(error_message_buff, sizeof(error_message_buff),
                ER_DEFAULT(ER_CANT_CREATE_THREAD), error);
    connect->close_with_error(ER_CANT_CREATE_THREAD, error_message_buff,
                              ER_OUT_OF_RESOURCES);
    DBUG_VOID_RETURN;
    /* purecov: end */
  }
  DBUG_PRINT("info",("Thread created"));
  DBUG_VOID_RETURN;
}


/**
  Create new thread to handle incoming connection.

    This function will create new thread to handle the incoming
    connection.  If there are idle cached threads one will be used.
    'thd' will be pushed into 'threads'.

    In single-threaded mode (\#define ONE_THREAD) connection will be
    handled inside this function.

  @param[in,out] thd    Thread handle of future thread.
*/

void create_new_thread(CONNECT *connect)
{
  DBUG_ENTER("create_new_thread");

  /*
    Don't allow too many connections. We roughly check here that we allow
    only (max_connections + 1) connections.
  */
  if ((*connect->scheduler->connection_count)++ >=
      *connect->scheduler->max_connections + 1)
  {
    DBUG_PRINT("error",("Too many connections"));
    connect->close_with_error(0, NullS, ER_CON_COUNT_ERROR);
    DBUG_VOID_RETURN;
  }

  uint sum= connection_count + extra_connection_count;
  if (sum > max_used_connections)
    max_used_connections= sum;

  /*
    The initialization of thread_id is done in create_embedded_thd() for
    the embedded library.
    TODO: refactor this to avoid code duplication there
  */
  connect->thread_id= next_thread_id();
  connect->scheduler->add_connection(connect);

  DBUG_VOID_RETURN;
}
#endif /* EMBEDDED_LIBRARY */


	/* Handle new connections and spawn new process to handle them */

#ifndef EMBEDDED_LIBRARY

void handle_accepted_socket(MYSQL_SOCKET new_sock, MYSQL_SOCKET sock)
{
#ifdef HAVE_LIBWRAP
  {
    if (!sock.is_unix_domain_socket)
    {
      struct request_info req;
      signal(SIGCHLD, SIG_DFL);
      request_init(&req, RQ_DAEMON, libwrapName, RQ_FILE,
        mysql_socket_getfd(new_sock), NULL);
      my_fromhost(&req);
      if (!my_hosts_access(&req))
      {
        /*
          This may be stupid but refuse() includes an exit(0)
          which we surely don't want...
          clean_exit() - same stupid thing ...
        */
        syslog(deny_severity, "refused connect from %s",
          my_eval_client(&req));

        /*
          C++ sucks (the gibberish in front just translates the supplied
          sink function pointer in the req structure from a void (*sink)();
          to a void(*sink)(int) if you omit the cast, the C++ compiler
          will cry...
        */
        if (req.sink)
          ((void(*)(int))req.sink)(req.fd);

        (void)mysql_socket_shutdown(new_sock, SHUT_RDWR);
        (void)mysql_socket_close(new_sock);
        /*
          The connection was refused by TCP wrappers.
          There are no details (by client IP) available to update the
          host_cache.
        */
        statistic_increment(connection_errors_tcpwrap, &LOCK_status);
        return;
      }
    }
  }
#endif /* HAVE_LIBWRAP */

  DBUG_PRINT("info", ("Creating CONNECT for new connection"));

  if (auto connect= new CONNECT(new_sock,
                                sock.is_unix_domain_socket ?
                                VIO_TYPE_SOCKET : VIO_TYPE_TCPIP,
                                sock.is_extra_port ?
                                extra_thread_scheduler : thread_scheduler))
    create_new_thread(connect);
  else
  {
    /* Connect failure */
    (void)mysql_socket_close(new_sock);
    statistic_increment(aborted_connects, &LOCK_status);
    statistic_increment(connection_errors_internal, &LOCK_status);
  }
}

#ifndef _WIN32
static void set_non_blocking_if_supported(MYSQL_SOCKET sock)
{
#if !defined(NO_FCNTL_NONBLOCK)
  if (!(test_flags & TEST_BLOCKING))
  {
    int flags= fcntl(mysql_socket_getfd(sock), F_GETFL, 0);
#if defined(O_NONBLOCK)
    fcntl(mysql_socket_getfd(sock), F_SETFL, flags | O_NONBLOCK);
#elif defined(O_NDELAY)
    fcntl(mysql_socket_getfd(sock), F_SETFL, flags | O_NDELAY);
#endif
  }
#endif
}


void handle_connections_sockets()
{
  MYSQL_SOCKET sock= mysql_socket_invalid();
  uint error_count=0;
  struct sockaddr_storage cAddr;
  int retval;
#ifdef HAVE_POLL
  // for ip_sock, unix_sock and extra_ip_sock
  Dynamic_array<struct pollfd> fds(PSI_INSTRUMENT_MEM);
#else
  fd_set readFDs,clientFDs;
#endif

  DBUG_ENTER("handle_connections_sockets");

#ifdef HAVE_POLL
  for (size_t i= 0; i < listen_sockets.size(); i++)
  {
    struct pollfd local_fds;
    mysql_socket_set_thread_owner(listen_sockets.at(i));
    local_fds.fd= mysql_socket_getfd(listen_sockets.at(i));
    local_fds.events= POLLIN;
    fds.push(local_fds);
    set_non_blocking_if_supported(listen_sockets.at(i));
  }
#else
  FD_ZERO(&clientFDs);
  for (size_t i= 0; i < listen_sockets.size(); i++)
  {
    int fd= mysql_socket_getfd(listen_sockets.at(i));
    FD_SET(fd, &clientFDs);
    set_non_blocking_if_supported(listen_sockets.at(i));
  }
#endif

  sd_notify(0, "READY=1\n"
            "STATUS=Taking your SQL requests now...\n");

  DBUG_PRINT("general",("Waiting for connections."));
  while (!abort_loop)
  {
#ifdef HAVE_POLL
    retval= poll(fds.get_pos(0), fds.size(), -1);
#else
    readFDs=clientFDs;
    retval= select(FD_SETSIZE, &readFDs, NULL, NULL, NULL);
#endif

    if (retval < 0)
    {
      if (socket_errno != SOCKET_EINTR)
      {
        /*
          select(2)/poll(2) failed on the listening port.
          There is not much details to report about the client,
          increment the server global status variable.
        */
        statistic_increment(connection_errors_accept, &LOCK_status);
	if (!select_errors++ && !abort_loop)	/* purecov: inspected */
	  sql_print_error("Server: Got error %d from select",socket_errno); /* purecov: inspected */
      }
      continue;
    }

    if (abort_loop)
      break;

    /* Is this a new connection request ? */
#ifdef HAVE_POLL
    for (size_t i= 0; i < fds.size(); ++i)
    {
      if (fds.at(i).revents & POLLIN)
      {
        sock= listen_sockets.at(i);
        break;
      }
    }
#else  // HAVE_POLL
    for (size_t i=0; i < listen_sockets.size(); i++)
    {
      if (FD_ISSET(mysql_socket_getfd(listen_sockets.at(i)), &readFDs))
      {
        sock= listen_sockets.at(i);
        break;
      }
    }
#endif // HAVE_POLL

    for (uint retry=0; retry < MAX_ACCEPT_RETRY; retry++)
    {
      size_socket length= sizeof(struct sockaddr_storage);
      MYSQL_SOCKET new_sock;

      new_sock= mysql_socket_accept(key_socket_client_connection, sock,
                                    (struct sockaddr *)(&cAddr),
                                    &length);
      if (mysql_socket_getfd(new_sock) != INVALID_SOCKET)
        handle_accepted_socket(new_sock, sock);
      else if (socket_errno != SOCKET_EINTR && socket_errno != SOCKET_EAGAIN)
      {
        /*
          accept(2) failed on the listening port.
          There is not much details to report about the client,
          increment the server global status variable.
        */
        statistic_increment(connection_errors_accept, &LOCK_status);
        if ((error_count++ & 255) == 0) // This can happen often
          sql_perror("Error in accept");
        if (socket_errno == SOCKET_ENFILE || socket_errno == SOCKET_EMFILE)
          sleep(1); // Give other threads some time
        break;
      }
    }
  }
  sd_notify(0, "STOPPING=1\n"
            "STATUS=Shutdown in progress\n");
  DBUG_VOID_RETURN;
}

#endif /* _WIN32*/
#endif /* EMBEDDED_LIBRARY */


/****************************************************************************
  Handle start options
******************************************************************************/


/**
  Process command line options flagged as 'early'.
  Some components needs to be initialized as early as possible,
  because the rest of the server initialization depends on them.
  Options that needs to be parsed early includes:
  - the performance schema, when compiled in,
  - options related to the help,
  - options related to the bootstrap
  The performance schema needs to be initialized as early as possible,
  before to-be-instrumented objects of the server are initialized.
*/

int handle_early_options()
{
  int ho_error;
  DYNAMIC_ARRAY all_early_options;

  /* Skip unknown options so that they may be processed later */
  my_getopt_skip_unknown= TRUE;

  /* prepare all_early_options array */
  my_init_dynamic_array(PSI_NOT_INSTRUMENTED, &all_early_options,
                        sizeof(my_option), 100, 25, MYF(0));
  add_many_options(&all_early_options, pfs_early_options,
                  array_elements(pfs_early_options));
  sys_var_add_options(&all_early_options, sys_var::PARSE_EARLY);
  add_terminator(&all_early_options);

  ho_error= handle_options(&remaining_argc, &remaining_argv,
                           (my_option*)(all_early_options.buffer),
                           mysqld_get_one_option);
  if (ho_error == 0)
  {
    /* Add back the program name handle_options removes */
    remaining_argc++;
    remaining_argv--;
  }

  delete_dynamic(&all_early_options);

  return ho_error;
}

/**
  System variables are automatically command-line options (few
  exceptions are documented in sys_var.h), so don't need
  to be listed here.
*/

struct my_option my_long_options[]=
{
  {"help", '?', "Display this help and exit.", 
   &opt_help, &opt_help, 0, GET_BOOL, NO_ARG, 0, 0, 0, 0,
   0, 0},
  {"allow-suspicious-udfs", 0,
   "Allows use of UDFs consisting of only one symbol xxx() "
   "without corresponding xxx_init() or xxx_deinit(). That also means "
   "that one can load any function from any library, for example exit() "
   "from libc.so",
   &opt_allow_suspicious_udfs, &opt_allow_suspicious_udfs,
   0, GET_BOOL, NO_ARG, 0, 0, 0, 0, 0, 0},
  {"ansi", 'a', "Use ANSI SQL syntax instead of MySQL syntax. This mode "
   "will also set transaction isolation level 'serializable'.", 0, 0, 0,
   GET_NO_ARG, NO_ARG, 0, 0, 0, 0, 0, 0},
  /*
    Because Sys_var_bit does not support command-line options, we need to
    explicitly add one for --autocommit
  */
  {"autocommit", 0, "Set default value for autocommit (0 or 1)",
   &opt_autocommit, &opt_autocommit, 0,
   GET_BOOL, OPT_ARG, 1, 0, 0, 0, 0, NULL},
  {"binlog-do-db", OPT_BINLOG_DO_DB,
   "Tells the master it should log updates for the specified database, "
   "and exclude all others not explicitly mentioned.",
   0, 0, 0, GET_STR, REQUIRED_ARG, 0, 0, 0, 0, 0, 0},
  {"binlog-ignore-db", OPT_BINLOG_IGNORE_DB,
   "Tells the master that updates to the given database should not be logged to the binary log.",
   0, 0, 0, GET_STR, REQUIRED_ARG, 0, 0, 0, 0, 0, 0},
  {"binlog-row-event-max-size", 0,
   "The maximum size of a row-based binary log event in bytes. Rows will be "
   "grouped into events smaller than this size if possible. "
   "The value has to be a multiple of 256.",
   &opt_binlog_rows_event_max_size, &opt_binlog_rows_event_max_size,
   0, GET_ULONG, REQUIRED_ARG,
   /* def_value */ 8192, /* min_value */  256, /* max_value */ UINT_MAX32-1,
   /* sub_size */     0, /* block_size */ 256,
   /* app_type */ 0
  },
#ifndef DISABLE_GRANT_OPTIONS
  {"bootstrap", OPT_BOOTSTRAP, "Used by mysql installation scripts.", 0, 0, 0,
   GET_NO_ARG, NO_ARG, 0, 0, 0, 0, 0, 0},
#endif
  {"character-set-client-handshake", 0,
   "Don't ignore client side character set value sent during handshake.",
   &opt_character_set_client_handshake,
   &opt_character_set_client_handshake,
    0, GET_BOOL, NO_ARG, 1, 0, 0, 0, 0, 0},
  {"character-set-filesystem", 0,
   "Set the filesystem character set.",
   &character_set_filesystem_name,
   &character_set_filesystem_name,
   0, GET_STR, REQUIRED_ARG, 0, 0, 0, 0, 0, 0 },
  {"character-set-server", 'C', "Set the default character set.",
   &default_character_set_name, &default_character_set_name,
   0, GET_STR, REQUIRED_ARG, 0, 0, 0, 0, 0, 0 },
  {"chroot", 'r', "Chroot mysqld daemon during startup.",
   &mysqld_chroot, &mysqld_chroot, 0, GET_STR, REQUIRED_ARG,
   0, 0, 0, 0, 0, 0},
  {"collation-server", 0, "Set the default collation.",
   &default_collation_name, &default_collation_name,
   0, GET_STR, REQUIRED_ARG, 0, 0, 0, 0, 0, 0 },
  {"console", OPT_CONSOLE, "Write error output on screen; don't remove the console window on windows.",
   &opt_console, &opt_console, 0, GET_BOOL, NO_ARG, 0, 0, 0,
   0, 0, 0},
  {"core-file", OPT_WANT_CORE, "Write core on errors.", 0, 0, 0, GET_NO_ARG,
   NO_ARG, 0, 0, 0, 0, 0, 0},
#ifdef DBUG_OFF
  {"debug", '#', "Built in DBUG debugger. Disabled in this build.",
   &current_dbug_option, &current_dbug_option, 0, GET_STR, OPT_ARG,
   0, 0, 0, 0, 0, 0},
#endif
#ifdef HAVE_REPLICATION
  {"debug-abort-slave-event-count", 0,
   "Option used by mysql-test for debugging and testing of replication.",
   &abort_slave_event_count,  &abort_slave_event_count,
   0, GET_INT, REQUIRED_ARG, 0, 0, 0, 0, 0, 0},
#endif /* HAVE_REPLICATION */
#ifndef DBUG_OFF
  {"debug-assert", 0,
   "Allow DBUG_ASSERT() to invoke assert()",
   &my_assert, &my_assert,
   0, GET_BOOL, OPT_ARG, 1, 0, 0, 0, 0, 0},
  {"debug-assert-on-error", 0,
   "Do an assert in various functions if we get a fatal error",
   &my_assert_on_error, &my_assert_on_error,
   0, GET_BOOL, NO_ARG, 0, 0, 0, 0, 0, 0},
  {"debug-assert-if-crashed-table", 0,
   "Do an assert in handler::print_error() if we get a crashed table",
   &debug_assert_if_crashed_table, &debug_assert_if_crashed_table,
   0, GET_BOOL, NO_ARG, 0, 0, 0, 0, 0, 0},
#endif
#ifdef HAVE_REPLICATION
  {"debug-disconnect-slave-event-count", 0,
   "Option used by mysql-test for debugging and testing of replication.",
   &disconnect_slave_event_count, &disconnect_slave_event_count,
   0, GET_INT, REQUIRED_ARG, 0, 0, 0, 0, 0, 0},
#endif /* HAVE_REPLICATION */
  {"debug-exit-info", 'T', "Used for debugging. Use at your own risk.",
   0, 0, 0, GET_LONG, OPT_ARG, 0, 0, 0, 0, 0, 0},
  {"debug-gdb", 0,
   "Set up signals usable for debugging.",
   &opt_debugging, &opt_debugging,
   0, GET_BOOL, NO_ARG, 0, 0, 0, 0, 0, 0},
#ifdef HAVE_REPLICATION
  {"debug-max-binlog-dump-events", 0,
   "Option used by mysql-test for debugging and testing of replication.",
   &max_binlog_dump_events, &max_binlog_dump_events, 0,
   GET_INT, REQUIRED_ARG, 0, 0, 0, 0, 0, 0},
#endif /* HAVE_REPLICATION */
  {"debug-no-sync", 0,
   "Disables system sync calls. Only for running tests or debugging!",
   &my_disable_sync, &my_disable_sync, 0, GET_BOOL, NO_ARG, 0, 0, 0, 0, 0, 0},
#ifdef HAVE_REPLICATION
  {"debug-sporadic-binlog-dump-fail", 0,
   "Option used by mysql-test for debugging and testing of replication.",
   &opt_sporadic_binlog_dump_fail,
   &opt_sporadic_binlog_dump_fail, 0, GET_BOOL, NO_ARG, 0, 0, 0, 0, 0,
   0},
#endif /* HAVE_REPLICATION */
#ifndef DBUG_OFF
  {"debug-assert-on-not-freed-memory", 0,
   "Assert if we found problems with memory allocation",
   &debug_assert_on_not_freed_memory,
   &debug_assert_on_not_freed_memory, 0, GET_BOOL, NO_ARG, 1, 0, 0, 0, 0,
   0},
#endif /* DBUG_OFF */
  /* default-storage-engine should have "MyISAM" as def_value. Instead
     of initializing it here it is done in init_common_variables() due
     to a compiler bug in Sun Studio compiler. */
  {"default-storage-engine", 0, "The default storage engine for new tables",
   &default_storage_engine, 0, 0, GET_STR, REQUIRED_ARG,
   0, 0, 0, 0, 0, 0 },
  {"default-tmp-storage-engine", 0,
    "The default storage engine for user-created temporary tables",
   &default_tmp_storage_engine, 0, 0, GET_STR, REQUIRED_ARG,
   0, 0, 0, 0, 0, 0 },
  {"default-time-zone", 0, "Set the default time zone.",
   &default_tz_name, &default_tz_name,
   0, GET_STR, REQUIRED_ARG, 0, 0, 0, 0, 0, 0 },
#if defined(ENABLED_DEBUG_SYNC)
  {"debug-sync-timeout", OPT_DEBUG_SYNC_TIMEOUT,
   "Enable the debug sync facility "
   "and optionally specify a default wait timeout in seconds. "
   "A zero value keeps the facility disabled.",
   &opt_debug_sync_timeout, 0,
   0, GET_UINT, OPT_ARG, 0, 0, UINT_MAX, 0, 0, 0},
#endif /* defined(ENABLED_DEBUG_SYNC) */
#ifdef HAVE_OPENSSL
  {"des-key-file", 0,
   "Load keys for des_encrypt() and des_encrypt from given file.",
   &des_key_file, &des_key_file, 0, GET_STR, REQUIRED_ARG,
   0, 0, 0, 0, 0, 0},
#endif /* HAVE_OPENSSL */
#ifdef HAVE_STACKTRACE
  {"stack-trace", 0 , "Print a symbolic stack trace on failure",
   &opt_stack_trace, &opt_stack_trace, 0, GET_BOOL, NO_ARG, 1, 0, 0, 0, 0, 0},
#endif /* HAVE_STACKTRACE */
  {"enforce-storage-engine", 0, "Force the use of a storage engine for new tables",
   &enforced_storage_engine, 0, 0, GET_STR, REQUIRED_ARG,
   0, 0, 0, 0, 0, 0 },
  {"external-locking", 0, "Use system (external) locking (disabled by "
   "default).  With this option enabled you can run myisamchk to test "
   "(not repair) tables while the MySQL server is running. Disable with "
   "--skip-external-locking.", &opt_external_locking, &opt_external_locking,
   0, GET_BOOL, NO_ARG, 0, 0, 0, 0, 0, 0},
  /* We must always support the next option to make scripts like mysqltest
     easier to do */
  {"flashback", 0,
   "Setup the server to use flashback. This enables binary log in row mode and will enable extra logging for DDL's needed by flashback feature",
   &opt_support_flashback, &opt_support_flashback,
   0, GET_BOOL, NO_ARG, 0, 0, 0, 0, 0, 0},
  {"gdb", 0,
   "Set up signals usable for debugging. Deprecated, use --debug-gdb instead.",
   &opt_debugging, &opt_debugging,
   0, GET_BOOL, NO_ARG, 0, 0, 0, 0, 0, 0},
  {"gtid-pos-auto-engines", 0,
   "List of engines for which to automatically create a "
   "mysql.gtid_slave_pos_ENGINE table, if a transaction using that engine "
   "is replicated. This can be used to avoid introducing cross-engine "
   "transactions, if engines are used different from that used by table "
   "mysql.gtid_slave_pos",
   &gtid_pos_auto_engines, 0, 0, GET_STR, REQUIRED_ARG,
   0, 0, 0, 0, 0, 0 },
#ifdef HAVE_SOLARIS_LARGE_PAGES
  {"super-large-pages", 0, "Enable support for super large pages.",
   &opt_super_large_pages, &opt_super_large_pages, 0,
   GET_BOOL, OPT_ARG, 0, 0, 1, 0, 1, 0},
#endif
  {"language", 'L',
   "Client error messages in given language. May be given as a full path. "
   "Deprecated. Use --lc-messages-dir instead.",
   0, 0, 0,
   GET_STR, REQUIRED_ARG, 0, 0, 0, 0, 0, 0},
  {"lc-messages", 0,
   "Set the language used for the error messages.",
   &lc_messages, &lc_messages, 0, GET_STR, REQUIRED_ARG,
   0, 0, 0, 0, 0, 0 },
  {"lc-time-names", 0,
   "Set the language used for the month names and the days of the week.",
   &lc_time_names_name, &lc_time_names_name,
   0, GET_STR, REQUIRED_ARG, 0, 0, 0, 0, 0, 0 },
  {"log-basename", OPT_LOG_BASENAME,
   "Basename for all log files and the .pid file. This sets all log file "
   "names at once (in 'datadir') and is normally the only option you need "
   "for specifying log files. Sets names for --log-bin, --log-bin-index, "
   "--relay-log, --relay-log-index, --general-log-file, "
   "--log-slow-query-log-file, --log-error-file, and --pid-file",
   &opt_log_basename, &opt_log_basename, 0, GET_STR, REQUIRED_ARG,
   0, 0, 0, 0, 0, 0},
  {"log-bin", OPT_BIN_LOG,
   "Log update queries in binary format. Optional argument should be name for "
   "binary log. If not given "
   "'datadir'/'log-basename'-bin or 'datadir'/mysql-bin will be used (the later if "
   "--log-basename is not specified). We strongly recommend to use either "
   "--log-basename or specify a filename to ensure that replication doesn't "
   "stop if the real hostname of the computer changes.",
   &opt_bin_logname, &opt_bin_logname, 0, GET_STR,
   OPT_ARG, 0, 0, 0, 0, 0, 0},
  {"log-bin-index", 0,
   "File that holds the names for last binary log files.",
   &opt_binlog_index_name, &opt_binlog_index_name, 0, GET_STR,
   REQUIRED_ARG, 0, 0, 0, 0, 0, 0},
  {"relay-log-index", 0,
   "The location and name to use for the file that keeps a list of the last "
   "relay logs",
   &opt_relaylog_index_name, &opt_relaylog_index_name, 0, GET_STR,
   REQUIRED_ARG, 0, 0, 0, 0, 0, 0},
  {"log-ddl-recovery", 0,
   "Path to file used for recovery of DDL statements after a crash",
   &opt_ddl_recovery_file, &opt_ddl_recovery_file, 0, GET_STR,
   REQUIRED_ARG, 0, 0, 0, 0, 0, 0},
  {"log-isam", OPT_ISAM_LOG, "Log all MyISAM changes to file.",
   &myisam_log_filename, &myisam_log_filename, 0, GET_STR,
   OPT_ARG, 0, 0, 0, 0, 0, 0},
  {"log-short-format", 0,
   "Don't log extra information to update and slow-query logs.",
   &opt_short_log_format, &opt_short_log_format,
   0, GET_BOOL, NO_ARG, 0, 0, 0, 0, 0, 0},
  {"log-tc", 0,
   "Path to transaction coordinator log (used for transactions that affect "
   "more than one storage engine, when binary log is disabled).",
   &opt_tc_log_file, &opt_tc_log_file, 0, GET_STR,
   REQUIRED_ARG, 0, 0, 0, 0, 0, 0},
  {"master-info-file", 0,
   "The location and name of the file that remembers the master and where "
   "the I/O replication thread is in the master's binlogs. Defaults to "
   "master.info",
   &master_info_file, &master_info_file, 0, GET_STR,
   REQUIRED_ARG, 0, 0, 0, 0, 0, 0},
  {"master-retry-count", 0,
   "The number of tries the slave will make to connect to the master before giving up.",
   &master_retry_count, &master_retry_count, 0, GET_ULONG,
   REQUIRED_ARG, 100000, 0, 0, 0, 0, 0},
#ifdef HAVE_REPLICATION
  {"init-rpl-role", 0, "Set the replication role",
   &rpl_status, &rpl_status, &rpl_role_typelib,
   GET_ENUM, REQUIRED_ARG, 0, 0, 0, 0, 0, 0},
#endif /* HAVE_REPLICATION */
  {"memlock", 0, "Lock mysqld in memory.", &locked_in_memory,
   &locked_in_memory, 0, GET_BOOL, NO_ARG, 0, 0, 0, 0, 0, 0},
  {"old-style-user-limits", 0,
   "Enable old-style user limits (before 5.0.3, user resources were counted "
   "per each user+host vs. per account).",
   &opt_old_style_user_limits, &opt_old_style_user_limits,
   0, GET_BOOL, NO_ARG, 0, 0, 0, 0, 0, 0},
  {"port-open-timeout", 0,
   "Maximum time in seconds to wait for the port to become free. "
   "(Default: No wait).", &mysqld_port_timeout, &mysqld_port_timeout, 0,
   GET_UINT, REQUIRED_ARG, 0, 0, 0, 0, 0, 0},
  {"replicate-do-db", OPT_REPLICATE_DO_DB,
   "Tells the slave thread to restrict replication to the specified database. "
   "To specify more than one database, use the directive multiple times, "
   "once for each database. Note that this will only work if you do not use "
   "cross-database queries such as UPDATE some_db.some_table SET foo='bar' "
   "while having selected a different or no database. If you need cross "
   "database updates to work, make sure you have 3.23.28 or later, and use "
   "replicate-wild-do-table=db_name.%.",
   0, 0, 0, GET_STR | GET_ASK_ADDR, REQUIRED_ARG, 0, 0, 0, 0, 0, 0},
  {"replicate-do-table", OPT_REPLICATE_DO_TABLE,
   "Tells the slave thread to restrict replication to the specified table. "
   "To specify more than one table, use the directive multiple times, once "
   "for each table. This will work for cross-database updates, in contrast "
   "to replicate-do-db.", 0, 0, 0, GET_STR | GET_ASK_ADDR, REQUIRED_ARG, 0, 0, 0, 0, 0, 0},
  {"replicate-ignore-db", OPT_REPLICATE_IGNORE_DB,
   "Tells the slave thread to not replicate to the specified database. To "
   "specify more than one database to ignore, use the directive multiple "
   "times, once for each database. This option will not work if you use "
   "cross database updates. If you need cross database updates to work, "
   "make sure you have 3.23.28 or later, and use replicate-wild-ignore-"
   "table=db_name.%. ", 0, 0, 0, GET_STR | GET_ASK_ADDR, REQUIRED_ARG, 0, 0, 0, 0, 0, 0},
  {"replicate-ignore-table", OPT_REPLICATE_IGNORE_TABLE,
   "Tells the slave thread to not replicate to the specified table. To specify "
   "more than one table to ignore, use the directive multiple times, once for "
   "each table. This will work for cross-database updates, in contrast to "
   "replicate-ignore-db.", 0, 0, 0, GET_STR | GET_ASK_ADDR, REQUIRED_ARG, 0, 0, 0, 0, 0, 0},
  {"replicate-rewrite-db", OPT_REPLICATE_REWRITE_DB,
   "Updates to a database with a different name than the original. Example: "
   "replicate-rewrite-db=master_db_name->slave_db_name.",
   0, 0, 0, GET_STR | GET_ASK_ADDR, REQUIRED_ARG, 0, 0, 0, 0, 0, 0},
#ifdef HAVE_REPLICATION
  {"replicate-same-server-id", 0,
   "In replication, if set to 1, do not skip events having our server id. "
   "Default value is 0 (to break infinite loops in circular replication). "
   "Can't be set to 1 if --log-slave-updates is used.",
   &replicate_same_server_id, &replicate_same_server_id,
   0, GET_BOOL, NO_ARG, 0, 0, 0, 0, 0, 0},
#endif
  {"replicate-wild-do-table", OPT_REPLICATE_WILD_DO_TABLE,
   "Tells the slave thread to restrict replication to the tables that match "
   "the specified wildcard pattern. To specify more than one table, use the "
   "directive multiple times, once for each table. This will work for cross-"
   "database updates. Example: replicate-wild-do-table=foo%.bar% will "
   "replicate only updates to tables in all databases that start with foo "
   "and whose table names start with bar.",
   0, 0, 0, GET_STR | GET_ASK_ADDR, REQUIRED_ARG, 0, 0, 0, 0, 0, 0},
  {"replicate-wild-ignore-table", OPT_REPLICATE_WILD_IGNORE_TABLE,
   "Tells the slave thread to not replicate to the tables that match the "
   "given wildcard pattern. To specify more than one table to ignore, use "
   "the directive multiple times, once for each table. This will work for "
   "cross-database updates. Example: replicate-wild-ignore-table=foo%.bar% "
   "will not do updates to tables in databases that start with foo and whose "
   "table names start with bar.",
   0, 0, 0, GET_STR | GET_ASK_ADDR, REQUIRED_ARG, 0, 0, 0, 0, 0, 0},
  {"safe-mode", OPT_SAFE, "Skip some optimize stages (for testing). Deprecated.",
   0, 0, 0, GET_NO_ARG, NO_ARG, 0, 0, 0, 0, 0, 0},
  {"safe-user-create", 0,
   "Don't allow new user creation by the user who has no write privileges to the mysql.user table.",
   &opt_safe_user_create, &opt_safe_user_create, 0, GET_BOOL,
   NO_ARG, 0, 0, 0, 0, 0, 0},
  {"show-slave-auth-info", 0,
   "Show user and password in SHOW SLAVE HOSTS on this master.",
   &opt_show_slave_auth_info, &opt_show_slave_auth_info, 0,
   GET_BOOL, NO_ARG, 0, 0, 0, 0, 0, 0},
  {"silent-startup", OPT_SILENT, "Don't print [Note] to the error log during startup.",
   &opt_silent_startup, &opt_silent_startup, 0, GET_BOOL, NO_ARG, 0, 0, 0, 0, 0, 0},
#ifndef DISABLE_GRANT_OPTIONS
  {"skip-grant-tables", 0,
   "Start without grant tables. This gives all users FULL ACCESS to all tables.",
   &opt_noacl, &opt_noacl, 0, GET_BOOL, NO_ARG, 0, 0, 0, 0, 0,
   0},
#endif
  {"skip-host-cache", OPT_SKIP_HOST_CACHE, "Don't cache host names.", 0, 0, 0,
   GET_NO_ARG, NO_ARG, 0, 0, 0, 0, 0, 0},
  {"skip-slave-start", 0,
   "If set, slave is not autostarted.", &opt_skip_slave_start,
   &opt_skip_slave_start, 0, GET_BOOL, NO_ARG, 0, 0, 0, 0, 0, 0},
#ifdef HAVE_REPLICATION
  {"slave-parallel-mode", OPT_SLAVE_PARALLEL_MODE,
   "Controls what transactions are applied in parallel when using "
   "--slave-parallel-threads. Possible values: \"optimistic\" tries to "
   "apply most transactional DML in parallel, and handles any conflicts "
   "with rollback and retry. \"conservative\" limits parallelism in an "
   "effort to avoid any conflicts. \"aggressive\" tries to maximise the "
   "parallelism, possibly at the cost of increased conflict rate. "
   "\"minimal\" only parallelizes the commit steps of transactions. "
   "\"none\" disables parallel apply completely.",
   &opt_slave_parallel_mode, &opt_slave_parallel_mode,
   &slave_parallel_mode_typelib, GET_ENUM | GET_ASK_ADDR, REQUIRED_ARG,
   SLAVE_PARALLEL_CONSERVATIVE, 0, 0, 0, 0, 0},
#endif
#if defined(_WIN32) && !defined(EMBEDDED_LIBRARY)
  {"slow-start-timeout", 0,
   "Maximum number of milliseconds that the service control manager should wait "
   "before trying to kill the windows service during startup"
   "(Default: 15000).", &slow_start_timeout, &slow_start_timeout, 0,
   GET_ULONG, REQUIRED_ARG, 15000, 0, 0, 0, 0, 0},
#endif
#ifdef HAVE_OPENSSL
  {"ssl", 0,
   "Enable SSL for connection (automatically enabled if an ssl option is used).",
   &opt_use_ssl, &opt_use_ssl, 0, GET_BOOL, OPT_ARG, 0, 0, 0,
   0, 0, 0},
#endif
#ifdef _WIN32
  {"standalone", 0,
  "Dummy option to start as a standalone program (NT).", 0, 0, 0, GET_NO_ARG,
   NO_ARG, 0, 0, 0, 0, 0, 0},
#endif
  {"symbolic-links", 's', "Enable symbolic link support.",
   &my_use_symdir, &my_use_symdir, 0, GET_BOOL, NO_ARG,
   /*
     The system call realpath() produces warnings under valgrind and
     purify. These are not suppressed: instead we disable symlinks
     option if compiled with valgrind support.
     Also disable by default on Windows, due to high overhead for checking .sym 
     files.
   */
   IF_WIN(0,1), 0, 0, 0, 0, 0},
  {"sysdate-is-now", 0,
   "Non-default option to alias SYSDATE() to NOW() to make it safe-replicable. "
   "Since 5.0, SYSDATE() returns a `dynamic' value different for different "
   "invocations, even within the same statement.",
   &global_system_variables.sysdate_is_now,
   0, 0, GET_BOOL, NO_ARG, 0, 0, 1, 0, 1, 0},
  {"tc-heuristic-recover", 0,
   "Decision to use in heuristic recover process",
   &tc_heuristic_recover, &tc_heuristic_recover,
   &tc_heuristic_recover_typelib, GET_ENUM, REQUIRED_ARG, 0, 0, 0, 0, 0, 0},
  {"temp-pool", 0,
#if (ENABLE_TEMP_POOL)
   "Using this option will cause most temporary files created to use a small "
   "set of names, rather than a unique name for each new file. Deprecated.",
#else
   "This option is ignored on this OS.",
#endif
   &use_temp_pool, &use_temp_pool, 0, GET_BOOL, NO_ARG, 0,
   0, 0, 0, 0, 0},
  {"transaction-isolation", 0,
   "Default transaction isolation level",
   &global_system_variables.tx_isolation,
   &global_system_variables.tx_isolation, &tx_isolation_typelib,
   GET_ENUM, REQUIRED_ARG, ISO_REPEATABLE_READ, 0, 0, 0, 0, 0},
  {"transaction-read-only", 0,
   "Default transaction access mode. "
   "True if transactions are read-only.",
   &global_system_variables.tx_read_only,
   &global_system_variables.tx_read_only, 0,
   GET_BOOL, OPT_ARG, 0, 0, 0, 0, 0, 0},
  {"user", 'u', "Run mysqld daemon as user.", 0, 0, 0, GET_STR, REQUIRED_ARG,
   0, 0, 0, 0, 0, 0},
  {"verbose", 'v', "Used with --help option for detailed help.",
   &opt_verbose, &opt_verbose, 0, GET_BOOL, NO_ARG, 0, 0, 0, 0, 0, 0},
  {"version", 'V', "Output version information and exit.", 0, 0, 0, GET_STR,
   OPT_ARG, 0, 0, 0, 0, 0, 0},
  {"plugin-load", OPT_PLUGIN_LOAD,
   "Semicolon-separated list of plugins to load, where each plugin is "
   "specified as ether a plugin_name=library_file pair or only a library_file. "
   "If the latter case, all plugins from a given library_file will be loaded.",
   0, 0, 0,
   GET_STR, REQUIRED_ARG, 0, 0, 0, 0, 0, 0},
  {"plugin-load-add", OPT_PLUGIN_LOAD_ADD,
   "Optional semicolon-separated list of plugins to load. This option adds "
   "to the list specified by --plugin-load in an incremental way. "
   "It can be specified many times, adding more plugins every time.",
   0, 0, 0,
    GET_STR, REQUIRED_ARG, 0, 0, 0, 0, 0, 0},
  {"table_cache", 0, "Deprecated; use --table-open-cache instead.",
   &tc_size, &tc_size, 0, GET_ULONG,
   REQUIRED_ARG, TABLE_OPEN_CACHE_DEFAULT, 1, 512*1024L, 0, 1, 0},
#ifdef WITH_WSREP
  {"wsrep-new-cluster", 0, "Bootstrap a cluster. It works by overriding the "
   "current value of wsrep_cluster_address. It is recommended not to add this "
   "option to the config file as this will trigger bootstrap on every server "
   "start.", &wsrep_new_cluster, &wsrep_new_cluster, 0, GET_BOOL, NO_ARG,
   0, 0, 0, 0, 0, 0},
#endif
};

static int show_queries(THD *thd, SHOW_VAR *var, char *buff,
                        enum enum_var_type scope)
{
  var->type= SHOW_LONGLONG;
  var->value= &thd->query_id;
  return 0;
}


static int show_net_compression(THD *thd, SHOW_VAR *var, char *buff,
                                enum enum_var_type scope)
{
  var->type= SHOW_MY_BOOL;
  var->value= &thd->net.compress;
  return 0;
}

static int show_starttime(THD *thd, SHOW_VAR *var, char *buff,
                          enum enum_var_type scope)
{
  var->type= SHOW_LONG;
  var->value= buff;
  *((long *)buff)= (long) (thd->query_start() - server_start_time);
  return 0;
}

#ifdef ENABLED_PROFILING
static int show_flushstatustime(THD *thd, SHOW_VAR *var, char *buff,
                                enum enum_var_type scope)
{
  var->type= SHOW_LONG;
  var->value= buff;
  *((long *)buff)= (long) (thd->query_start() - flush_status_time);
  return 0;
}
#endif

#ifdef HAVE_REPLICATION
static int show_rpl_status(THD *thd, SHOW_VAR *var, char *buff,
                           enum enum_var_type scope)
{
  var->type= SHOW_CHAR;
  var->value= const_cast<char*>(rpl_status_type[(int)rpl_status]);
  return 0;
}

static int show_slave_running(THD *thd, SHOW_VAR *var, char *buff,
                              enum enum_var_type scope)
{
  Master_info *mi= NULL;
  bool UNINIT_VAR(tmp);

  var->type= SHOW_MY_BOOL;
  var->value= buff;

  if ((mi= get_master_info(&thd->variables.default_master_connection,
                           Sql_condition::WARN_LEVEL_NOTE)))
  {
    tmp= (my_bool) (mi->slave_running == MYSQL_SLAVE_RUN_READING &&
                    mi->rli.slave_running != MYSQL_SLAVE_NOT_RUN);
    mi->release();
  }
  if (mi)
    *((my_bool *)buff)= tmp;
  else
    var->type= SHOW_UNDEF;
  return 0;
}


/* How many masters this slave is connected to */


static int show_slaves_running(THD *thd, SHOW_VAR *var, char *buff)
{
  var->type= SHOW_LONGLONG;
  var->value= buff;

  *((longlong *)buff)= any_slave_sql_running(false);

  return 0;
}


static int show_slave_received_heartbeats(THD *thd, SHOW_VAR *var, char *buff,
                                          enum enum_var_type scope)
{
  Master_info *mi;

  var->type= SHOW_LONGLONG;
  var->value= buff;

  if ((mi= get_master_info(&thd->variables.default_master_connection,
                           Sql_condition::WARN_LEVEL_NOTE)))
  {
    *((longlong *)buff)= mi->received_heartbeats;
    mi->release();
  }
  else
    var->type= SHOW_UNDEF;
  return 0;
}


static int show_heartbeat_period(THD *thd, SHOW_VAR *var, char *buff,
                                 enum enum_var_type scope)
{
  Master_info *mi;

  var->type= SHOW_CHAR;
  var->value= buff;

  if ((mi= get_master_info(&thd->variables.default_master_connection,
                           Sql_condition::WARN_LEVEL_NOTE)))
  {
    sprintf(buff, "%.3f", mi->heartbeat_period);
    mi->release();
  }
  else
    var->type= SHOW_UNDEF;
  return 0;
}


#endif /* HAVE_REPLICATION */

static int show_open_tables(THD *thd, SHOW_VAR *var, char *buff,
                            enum enum_var_type scope)
{
  var->type= SHOW_LONG;
  var->value= buff;
  *((long *) buff)= (long) tc_records();
  return 0;
}

static int show_prepared_stmt_count(THD *thd, SHOW_VAR *var, char *buff,
                                    enum enum_var_type scope)
{
  var->type= SHOW_LONG;
  var->value= buff;
  mysql_mutex_lock(&LOCK_prepared_stmt_count);
  *((long *)buff)= (long)prepared_stmt_count;
  mysql_mutex_unlock(&LOCK_prepared_stmt_count);
  return 0;
}

static int show_table_definitions(THD *thd, SHOW_VAR *var, char *buff,
                                  enum enum_var_type scope)
{
  var->type= SHOW_LONG;
  var->value= buff;
  *((long *) buff)= (long) tdc_records();
  return 0;
}


#if defined(HAVE_OPENSSL) && !defined(EMBEDDED_LIBRARY)

/*
   Functions relying on SSL
   Note: In the show_ssl_* functions, we need to check if we have a
         valid vio-object since this isn't always true, specifically
         when session_status or global_status is requested from
         inside an Event.
 */

static int show_ssl_get_version(THD *thd, SHOW_VAR *var, char *buff,
                                enum enum_var_type scope)
{
  var->type= SHOW_CHAR;
  if( thd->vio_ok() && thd->net.vio->ssl_arg )
    var->value= const_cast<char*>(SSL_get_version((SSL*) thd->net.vio->ssl_arg));
  else
    var->value= const_cast<char*>("");
  return 0;
}

static int show_ssl_get_default_timeout(THD *thd, SHOW_VAR *var, char *buff,
                                        enum enum_var_type scope)
{
  var->type= SHOW_LONG;
  var->value= buff;
  if( thd->vio_ok() && thd->net.vio->ssl_arg )
    *((long *)buff)= (long)SSL_get_default_timeout((SSL*)thd->net.vio->ssl_arg);
  else
    *((long *)buff)= 0;
  return 0;
}

static int show_ssl_get_verify_mode(THD *thd, SHOW_VAR *var, char *buff,
                                    enum enum_var_type scope)
{
  var->type= SHOW_LONG;
  var->value= buff;
#ifndef HAVE_WOLFSSL
  if( thd->net.vio && thd->net.vio->ssl_arg )
    *((long *)buff)= (long)SSL_get_verify_mode((SSL*)thd->net.vio->ssl_arg);
  else
    *((long *)buff)= 0;
#else
  *((long *)buff)= 0;
#endif
  return 0;
}

static int show_ssl_get_verify_depth(THD *thd, SHOW_VAR *var, char *buff,
                                     enum enum_var_type scope)
{
  var->type= SHOW_LONG;
  var->value= buff;
  if( thd->vio_ok() && thd->net.vio->ssl_arg )
    *((long *)buff)= (long)SSL_get_verify_depth((SSL*)thd->net.vio->ssl_arg);
  else
    *((long *)buff)= 0;

  return 0;
}

static int show_ssl_get_cipher(THD *thd, SHOW_VAR *var, char *buff,
                               enum enum_var_type scope)
{
  var->type= SHOW_CHAR;
  if( thd->vio_ok() && thd->net.vio->ssl_arg )
    var->value= const_cast<char*>(SSL_get_cipher((SSL*) thd->net.vio->ssl_arg));
  else
    var->value= const_cast<char*>("");
  return 0;
}

static int show_ssl_get_cipher_list(THD *thd, SHOW_VAR *var, char *buff,
                                    enum enum_var_type scope)
{
  var->type= SHOW_CHAR;
  var->value= buff;
  if (thd->vio_ok() && thd->net.vio->ssl_arg)
  {
    int i;
    const char *p;
    char *end= buff + SHOW_VAR_FUNC_BUFF_SIZE;
    for (i=0; (p= SSL_get_cipher_list((SSL*) thd->net.vio->ssl_arg,i)) &&
               buff < end; i++)
    {
      buff= strnmov(buff, p, end-buff-1);
      *buff++= ':';
    }
    if (i)
      buff--;
  }
  *buff=0;
  return 0;
}

#define SHOW_FNAME(name)                        \
    rpl_semi_sync_master_show_##name

#define DEF_SHOW_FUNC(name, show_type)                                       \
    static  int SHOW_FNAME(name)(MYSQL_THD thd, SHOW_VAR *var, char *buff)   \
    {                                                                        \
      repl_semisync_master.set_export_stats();                                 \
      var->type= show_type;                                                  \
      var->value= (char *)&rpl_semi_sync_master_##name;                      \
      return 0;                                                              \
    }

DEF_SHOW_FUNC(status, SHOW_BOOL)
DEF_SHOW_FUNC(clients, SHOW_LONG)
DEF_SHOW_FUNC(wait_sessions, SHOW_LONG)
DEF_SHOW_FUNC(trx_wait_time, SHOW_LONGLONG)
DEF_SHOW_FUNC(trx_wait_num, SHOW_LONGLONG)
DEF_SHOW_FUNC(net_wait_time, SHOW_LONGLONG)
DEF_SHOW_FUNC(net_wait_num, SHOW_LONGLONG)
DEF_SHOW_FUNC(avg_net_wait_time, SHOW_LONG)
DEF_SHOW_FUNC(avg_trx_wait_time, SHOW_LONG)


static char *
my_asn1_time_to_string(const ASN1_TIME *time, char *buf, size_t len)
{
  int n_read;
  char *res= NULL;
  BIO *bio= BIO_new(BIO_s_mem());

  if (bio == NULL)
    return NULL;

  if (!ASN1_TIME_print(bio, const_cast<ASN1_TIME*>(time)))
    goto end;

  n_read= BIO_read(bio, buf, (int) (len - 1));

  if (n_read > 0)
  {
    buf[n_read]= 0;
    res= buf;
  }

end:
  BIO_free(bio);
  return res;
}


/**
  Handler function for the 'ssl_get_server_not_before' variable

  @param      thd  the mysql thread structure
  @param      var  the data for the variable
  @param[out] buf  the string to put the value of the variable into

  @return          status
  @retval     0    success
*/

static int
show_ssl_get_server_not_before(THD *thd, SHOW_VAR *var, char *buff,
                               enum enum_var_type scope)
{
  var->type= SHOW_CHAR;
  if(thd->vio_ok() && thd->net.vio->ssl_arg)
  {
    SSL *ssl= (SSL*) thd->net.vio->ssl_arg;
    X509 *cert= SSL_get_certificate(ssl);
    const ASN1_TIME *not_before= X509_get0_notBefore(cert);

    var->value= my_asn1_time_to_string(not_before, buff,
                                       SHOW_VAR_FUNC_BUFF_SIZE);
    if (!var->value)
      return 1;
    var->value= buff;
  }
  else
    var->value= empty_c_string;
  return 0;
}


/**
  Handler function for the 'ssl_get_server_not_after' variable

  @param      thd  the mysql thread structure
  @param      var  the data for the variable
  @param[out] buf  the string to put the value of the variable into

  @return          status
  @retval     0    success
*/

static int
show_ssl_get_server_not_after(THD *thd, SHOW_VAR *var, char *buff,
                              enum enum_var_type scope)
{
  var->type= SHOW_CHAR;
  if(thd->vio_ok() && thd->net.vio->ssl_arg)
  {
    SSL *ssl= (SSL*) thd->net.vio->ssl_arg;
    X509 *cert= SSL_get_certificate(ssl);
    const ASN1_TIME *not_after= X509_get0_notAfter(cert);

    var->value= my_asn1_time_to_string(not_after, buff,
                                       SHOW_VAR_FUNC_BUFF_SIZE);
    if (!var->value)
      return 1;
  }
  else
    var->value= empty_c_string;
  return 0;
}

#endif /* HAVE_OPENSSL && !EMBEDDED_LIBRARY */

static int show_default_keycache(THD *thd, SHOW_VAR *var, void *buff,
                                 system_status_var *, enum_var_type)
{
  struct st_data {
    KEY_CACHE_STATISTICS stats;
    SHOW_VAR var[9];
  } *data;
  SHOW_VAR *v;

  data=(st_data *)buff;
  v= data->var;

  var->type= SHOW_ARRAY;
  var->value= v;

  get_key_cache_statistics(dflt_key_cache, 0, &data->stats);

#define set_one_keycache_var(X,Y)       \
  v->name= X;                           \
  v->type= SHOW_LONGLONG;               \
  v->value= &data->stats.Y;      \
  v++;

  set_one_keycache_var("blocks_not_flushed", blocks_changed);
  set_one_keycache_var("blocks_unused",      blocks_unused);
  set_one_keycache_var("blocks_used",        blocks_used);
  set_one_keycache_var("blocks_warm",        blocks_warm);
  set_one_keycache_var("read_requests",      read_requests);
  set_one_keycache_var("reads",              reads);
  set_one_keycache_var("write_requests",     write_requests);
  set_one_keycache_var("writes",             writes);

  v->name= 0;

  DBUG_ASSERT((char*)(v+1) <= static_cast<char*>(buff) + SHOW_VAR_FUNC_BUFF_SIZE);

#undef set_one_keycache_var

  return 0;
}


static int show_memory_used(THD *thd, SHOW_VAR *var, char *buff,
                            struct system_status_var *status_var,
                            enum enum_var_type scope)
{
  var->type= SHOW_LONGLONG;
  var->value= buff;
  if (scope == OPT_GLOBAL)
  {
    calc_sum_of_all_status_if_needed(status_var);
    *(longlong*) buff= (status_var->global_memory_used +
                        status_var->local_memory_used);
  }
  else
    *(longlong*) buff= status_var->local_memory_used;
  return 0;
}


#ifndef DBUG_OFF
static int debug_status_func(THD *thd, SHOW_VAR *var, void *buff,
                             system_status_var *, enum_var_type)
{
#define add_var(X,Y,Z)                  \
  v->name= X;                           \
  v->value= (char*)Y;                   \
  v->type= Z;                           \
  v++;

  var->type= SHOW_ARRAY;
  var->value= buff;

  SHOW_VAR *v= (SHOW_VAR *)buff;

  if (_db_keyword_(0, "role_merge_stats", 1))
  {
    static SHOW_VAR roles[]= {
      {"global",  &role_global_merges,  SHOW_ULONG},
      {"db",      &role_db_merges,      SHOW_ULONG},
      {"table",   &role_table_merges,   SHOW_ULONG},
      {"column",  &role_column_merges,  SHOW_ULONG},
      {"routine", &role_routine_merges, SHOW_ULONG},
      {NullS, NullS, SHOW_LONG}
    };

    add_var("role_merges", roles, SHOW_ARRAY);
  }

  v->name= 0;

#undef add_var

  return 0;
}
#endif

#ifdef HAVE_POOL_OF_THREADS
static int show_threadpool_idle_threads(THD *thd, SHOW_VAR *var, char *buff,
                                 enum enum_var_type scope)
{
  var->type= SHOW_INT;
  var->value= buff;
  *(int *)buff= tp_get_idle_thread_count(); 
  return 0;
}


static int show_threadpool_threads(THD *thd, SHOW_VAR *var, char *buff,
                                   enum enum_var_type scope)
{
  var->type= SHOW_INT;
  var->value= buff;
  *(reinterpret_cast<int*>(buff))= tp_get_thread_count();
  return 0;
}
#endif


static int show_cached_thread_count(THD *thd, SHOW_VAR *var, char *buff,
                                    enum enum_var_type scope)
{
  var->type= SHOW_LONG;
  var->value= buff;
  *(reinterpret_cast<ulong*>(buff))= thread_cache.size();
  return 0;
}


/*
  Variables shown by SHOW STATUS in alphabetical order
*/

SHOW_VAR status_vars[]= {
  {"Aborted_clients",          (char*) &aborted_threads,        SHOW_LONG},
  {"Aborted_connects",         (char*) &aborted_connects,       SHOW_LONG},
  {"Aborted_connects_preauth", (char*) &aborted_connects_preauth, SHOW_LONG},
  {"Acl",                      (char*) acl_statistics,          SHOW_ARRAY},
  {"Access_denied_errors",     (char*) offsetof(STATUS_VAR, access_denied_errors), SHOW_LONG_STATUS},
  {"Binlog_bytes_written",     (char*) offsetof(STATUS_VAR, binlog_bytes_written), SHOW_LONGLONG_STATUS},
  {"Binlog_cache_disk_use",    (char*) &binlog_cache_disk_use,  SHOW_LONG},
  {"Binlog_cache_use",         (char*) &binlog_cache_use,       SHOW_LONG},
  {"Binlog_stmt_cache_disk_use",(char*) &binlog_stmt_cache_disk_use,  SHOW_LONG},
  {"Binlog_stmt_cache_use",    (char*) &binlog_stmt_cache_use,       SHOW_LONG},
  {"Busy_time",                (char*) offsetof(STATUS_VAR, busy_time), SHOW_DOUBLE_STATUS},
  {"Bytes_received",           (char*) offsetof(STATUS_VAR, bytes_received), SHOW_LONGLONG_STATUS},
  {"Bytes_sent",               (char*) offsetof(STATUS_VAR, bytes_sent), SHOW_LONGLONG_STATUS},
  {"Column_compressions",      (char*) offsetof(STATUS_VAR, column_compressions), SHOW_LONG_STATUS},
  {"Column_decompressions",    (char*) offsetof(STATUS_VAR, column_decompressions), SHOW_LONG_STATUS},
  {"Com",                      (char*) com_status_vars, SHOW_ARRAY},
  {"Compression",              (char*) &show_net_compression, SHOW_SIMPLE_FUNC},
  {"Connections",              (char*) &global_thread_id,         SHOW_LONG_NOFLUSH},
  {"Connection_errors_accept", (char*) &connection_errors_accept, SHOW_LONG},
  {"Connection_errors_internal", (char*) &connection_errors_internal, SHOW_LONG},
  {"Connection_errors_max_connections", (char*) &connection_errors_max_connection, SHOW_LONG},
  {"Connection_errors_peer_address", (char*) &connection_errors_peer_addr, SHOW_LONG},
  {"Connection_errors_select", (char*) &connection_errors_select, SHOW_LONG},
  {"Connection_errors_tcpwrap", (char*) &connection_errors_tcpwrap, SHOW_LONG},
  {"Cpu_time",                 (char*) offsetof(STATUS_VAR, cpu_time), SHOW_DOUBLE_STATUS},
  {"Created_tmp_disk_tables",  (char*) offsetof(STATUS_VAR, created_tmp_disk_tables_), SHOW_LONG_STATUS},
  {"Created_tmp_files",	       (char*) &my_tmp_file_created,	SHOW_LONG},
  {"Created_tmp_tables",       (char*) offsetof(STATUS_VAR, created_tmp_tables_), SHOW_LONG_STATUS},
#ifndef DBUG_OFF
  {"Debug",                    (char*) &debug_status_func,  SHOW_FUNC},
#endif
  {"Delayed_errors",           (char*) &delayed_insert_errors,  SHOW_LONG},
  {"Delayed_insert_threads",   (char*) &delayed_insert_threads, SHOW_LONG_NOFLUSH},
  {"Delayed_writes",           (char*) &delayed_insert_writes,  SHOW_LONG},
  {"Delete_scan",	       (char*) offsetof(STATUS_VAR, delete_scan_count), SHOW_LONG_STATUS},
  {"Empty_queries",            (char*) offsetof(STATUS_VAR, empty_queries), SHOW_LONG_STATUS},
  {"Executed_events",          (char*) &executed_events, SHOW_LONG_NOFLUSH },
  {"Executed_triggers",        (char*) offsetof(STATUS_VAR, executed_triggers), SHOW_LONG_STATUS},
  {"Feature_check_constraint", (char*) &feature_check_constraint, SHOW_LONG },
  {"Feature_custom_aggregate_functions", (char*) offsetof(STATUS_VAR, feature_custom_aggregate_functions), SHOW_LONG_STATUS},
  {"Feature_delay_key_write",  (char*) &feature_files_opened_with_delayed_keys, SHOW_LONG },
  {"Feature_dynamic_columns",  (char*) offsetof(STATUS_VAR, feature_dynamic_columns), SHOW_LONG_STATUS},
  {"Feature_fulltext",         (char*) offsetof(STATUS_VAR, feature_fulltext), SHOW_LONG_STATUS},
  {"Feature_gis",              (char*) offsetof(STATUS_VAR, feature_gis), SHOW_LONG_STATUS},
  {"Feature_insert_returning",   (char*)offsetof(STATUS_VAR, feature_insert_returning), SHOW_LONG_STATUS},
  {"Feature_invisible_columns",   (char*) offsetof(STATUS_VAR, feature_invisible_columns), SHOW_LONG_STATUS},
  {"Feature_json",             (char*) offsetof(STATUS_VAR, feature_json), SHOW_LONG_STATUS},
  {"Feature_locale",           (char*) offsetof(STATUS_VAR, feature_locale), SHOW_LONG_STATUS},
  {"Feature_subquery",         (char*) offsetof(STATUS_VAR, feature_subquery), SHOW_LONG_STATUS},
  {"Feature_system_versioning",   (char*) offsetof(STATUS_VAR, feature_system_versioning), SHOW_LONG_STATUS},
  {"Feature_application_time_periods", (char*) offsetof(STATUS_VAR, feature_application_time_periods), SHOW_LONG_STATUS},
  {"Feature_timezone",         (char*) offsetof(STATUS_VAR, feature_timezone), SHOW_LONG_STATUS},
  {"Feature_trigger",          (char*) offsetof(STATUS_VAR, feature_trigger), SHOW_LONG_STATUS},
  {"Feature_window_functions", (char*) offsetof(STATUS_VAR, feature_window_functions), SHOW_LONG_STATUS},
  {"Feature_xml",              (char*) offsetof(STATUS_VAR, feature_xml), SHOW_LONG_STATUS},
  {"Handler_commit",           (char*) offsetof(STATUS_VAR, ha_commit_count), SHOW_LONG_STATUS},
  {"Handler_delete",           (char*) offsetof(STATUS_VAR, ha_delete_count), SHOW_LONG_STATUS},
  {"Handler_discover",         (char*) offsetof(STATUS_VAR, ha_discover_count), SHOW_LONG_STATUS},
  {"Handler_external_lock",    (char*) offsetof(STATUS_VAR, ha_external_lock_count), SHOW_LONG_STATUS},
  {"Handler_icp_attempts",     (char*) offsetof(STATUS_VAR, ha_icp_attempts), SHOW_LONG_STATUS},
  {"Handler_icp_match",        (char*) offsetof(STATUS_VAR, ha_icp_match), SHOW_LONG_STATUS},
  {"Handler_mrr_init",         (char*) offsetof(STATUS_VAR, ha_mrr_init_count),  SHOW_LONG_STATUS},
  {"Handler_mrr_key_refills",  (char*) offsetof(STATUS_VAR, ha_mrr_key_refills_count), SHOW_LONG_STATUS},
  {"Handler_mrr_rowid_refills",(char*) offsetof(STATUS_VAR, ha_mrr_rowid_refills_count), SHOW_LONG_STATUS},
  {"Handler_prepare",          (char*) offsetof(STATUS_VAR, ha_prepare_count),  SHOW_LONG_STATUS},
  {"Handler_read_first",       (char*) offsetof(STATUS_VAR, ha_read_first_count), SHOW_LONG_STATUS},
  {"Handler_read_key",         (char*) offsetof(STATUS_VAR, ha_read_key_count), SHOW_LONG_STATUS},
  {"Handler_read_last",        (char*) offsetof(STATUS_VAR, ha_read_last_count), SHOW_LONG_STATUS},
  {"Handler_read_next",        (char*) offsetof(STATUS_VAR, ha_read_next_count), SHOW_LONG_STATUS},
  {"Handler_read_prev",        (char*) offsetof(STATUS_VAR, ha_read_prev_count), SHOW_LONG_STATUS},
  {"Handler_read_retry",       (char*) offsetof(STATUS_VAR, ha_read_retry_count), SHOW_LONG_STATUS},
  {"Handler_read_rnd",         (char*) offsetof(STATUS_VAR, ha_read_rnd_count), SHOW_LONG_STATUS},
  {"Handler_read_rnd_deleted", (char*) offsetof(STATUS_VAR, ha_read_rnd_deleted_count), SHOW_LONG_STATUS},
  {"Handler_read_rnd_next",    (char*) offsetof(STATUS_VAR, ha_read_rnd_next_count), SHOW_LONG_STATUS},
  {"Handler_rollback",         (char*) offsetof(STATUS_VAR, ha_rollback_count), SHOW_LONG_STATUS},
  {"Handler_savepoint",        (char*) offsetof(STATUS_VAR, ha_savepoint_count), SHOW_LONG_STATUS},
  {"Handler_savepoint_rollback",(char*) offsetof(STATUS_VAR, ha_savepoint_rollback_count), SHOW_LONG_STATUS},
  {"Handler_tmp_delete",       (char*) offsetof(STATUS_VAR, ha_tmp_delete_count), SHOW_LONG_STATUS},
  {"Handler_tmp_update",       (char*) offsetof(STATUS_VAR, ha_tmp_update_count), SHOW_LONG_STATUS},
  {"Handler_tmp_write",        (char*) offsetof(STATUS_VAR, ha_tmp_write_count), SHOW_LONG_STATUS},
  {"Handler_update",           (char*) offsetof(STATUS_VAR, ha_update_count), SHOW_LONG_STATUS},
  {"Handler_write",            (char*) offsetof(STATUS_VAR, ha_write_count), SHOW_LONG_STATUS},
  {"Key",                      (char*) &show_default_keycache, SHOW_FUNC},
  {"Last_query_cost",          (char*) offsetof(STATUS_VAR, last_query_cost), SHOW_DOUBLE_STATUS},
#ifndef DBUG_OFF
  {"malloc_calls",             (char*) &malloc_calls, SHOW_LONG},
#endif
  {"Max_statement_time_exceeded", (char*) offsetof(STATUS_VAR, max_statement_time_exceeded), SHOW_LONG_STATUS},
  {"Master_gtid_wait_count",   (char*) offsetof(STATUS_VAR, master_gtid_wait_count), SHOW_LONG_STATUS},
  {"Master_gtid_wait_timeouts", (char*) offsetof(STATUS_VAR, master_gtid_wait_timeouts), SHOW_LONG_STATUS},
  {"Master_gtid_wait_time",    (char*) offsetof(STATUS_VAR, master_gtid_wait_time), SHOW_LONG_STATUS},
  {"Max_used_connections",     (char*) &max_used_connections,  SHOW_LONG},
  {"Memory_used",              (char*) &show_memory_used, SHOW_SIMPLE_FUNC},
  {"Memory_used_initial",      (char*) &start_memory_used, SHOW_LONGLONG},
  {"Resultset_metadata_skipped", (char *) offsetof(STATUS_VAR, skip_metadata_count),SHOW_LONG_STATUS},
  {"Not_flushed_delayed_rows", (char*) &delayed_rows_in_use,    SHOW_LONG_NOFLUSH},
  {"Open_files",               (char*) &my_file_opened,         SHOW_SINT},
  {"Open_streams",             (char*) &my_stream_opened,       SHOW_LONG_NOFLUSH},
  {"Open_table_definitions",   (char*) &show_table_definitions, SHOW_SIMPLE_FUNC},
  {"Open_tables",              (char*) &show_open_tables,       SHOW_SIMPLE_FUNC},
  {"Opened_files",             (char*) &my_file_total_opened, SHOW_LONG_NOFLUSH},
  {"Opened_plugin_libraries",  (char*) &dlopen_count, SHOW_LONG},
  {"Opened_table_definitions", (char*) offsetof(STATUS_VAR, opened_shares), SHOW_LONG_STATUS},
  {"Opened_tables",            (char*) offsetof(STATUS_VAR, opened_tables), SHOW_LONG_STATUS},
  {"Opened_views",             (char*) offsetof(STATUS_VAR, opened_views), SHOW_LONG_STATUS},
  {"Prepared_stmt_count",      (char*) &show_prepared_stmt_count, SHOW_SIMPLE_FUNC},
  {"Rows_sent",                (char*) offsetof(STATUS_VAR, rows_sent), SHOW_LONGLONG_STATUS},
  {"Rows_read",                (char*) offsetof(STATUS_VAR, rows_read), SHOW_LONGLONG_STATUS},
  {"Rows_tmp_read",            (char*) offsetof(STATUS_VAR, rows_tmp_read), SHOW_LONGLONG_STATUS},
#ifdef HAVE_REPLICATION
  {"Rpl_semi_sync_master_status", (char*) &SHOW_FNAME(status), SHOW_FUNC},
  {"Rpl_semi_sync_master_clients", (char*) &SHOW_FNAME(clients), SHOW_FUNC},
  {"Rpl_semi_sync_master_yes_tx", (char*) &rpl_semi_sync_master_yes_transactions, SHOW_LONG},
  {"Rpl_semi_sync_master_no_tx", (char*) &rpl_semi_sync_master_no_transactions, SHOW_LONG},
  {"Rpl_semi_sync_master_wait_sessions", (char*) &SHOW_FNAME(wait_sessions), SHOW_FUNC},
  {"Rpl_semi_sync_master_no_times", (char*) &rpl_semi_sync_master_off_times, SHOW_LONG},
  {"Rpl_semi_sync_master_timefunc_failures", (char*) &rpl_semi_sync_master_timefunc_fails, SHOW_LONG},
  {"Rpl_semi_sync_master_wait_pos_backtraverse", (char*) &rpl_semi_sync_master_wait_pos_backtraverse, SHOW_LONG},
  {"Rpl_semi_sync_master_tx_wait_time", (char*) &SHOW_FNAME(trx_wait_time), SHOW_FUNC},
  {"Rpl_semi_sync_master_tx_waits", (char*) &SHOW_FNAME(trx_wait_num), SHOW_FUNC},
  {"Rpl_semi_sync_master_tx_avg_wait_time", (char*) &SHOW_FNAME(avg_trx_wait_time), SHOW_FUNC},
  {"Rpl_semi_sync_master_net_wait_time", (char*) &SHOW_FNAME(net_wait_time), SHOW_FUNC},
  {"Rpl_semi_sync_master_net_waits", (char*) &SHOW_FNAME(net_wait_num), SHOW_FUNC},
  {"Rpl_semi_sync_master_net_avg_wait_time", (char*) &SHOW_FNAME(avg_net_wait_time), SHOW_FUNC},
  {"Rpl_semi_sync_master_request_ack", (char*) &rpl_semi_sync_master_request_ack, SHOW_LONGLONG},
  {"Rpl_semi_sync_master_get_ack", (char*)&rpl_semi_sync_master_get_ack, SHOW_LONGLONG},
  {"Rpl_semi_sync_slave_status", (char*) &rpl_semi_sync_slave_status, SHOW_BOOL},
  {"Rpl_semi_sync_slave_send_ack", (char*) &rpl_semi_sync_slave_send_ack, SHOW_LONGLONG},
#endif /* HAVE_REPLICATION */
#ifdef HAVE_QUERY_CACHE
  {"Qcache_free_blocks",       (char*) &query_cache.free_memory_blocks, SHOW_LONG_NOFLUSH},
  {"Qcache_free_memory",       (char*) &query_cache.free_memory, SHOW_LONG_NOFLUSH},
  {"Qcache_hits",              (char*) &query_cache.hits,       SHOW_LONG},
  {"Qcache_inserts",           (char*) &query_cache.inserts,    SHOW_LONG},
  {"Qcache_lowmem_prunes",     (char*) &query_cache.lowmem_prunes, SHOW_LONG},
  {"Qcache_not_cached",        (char*) &query_cache.refused,    SHOW_LONG},
  {"Qcache_queries_in_cache",  (char*) &query_cache.queries_in_cache, SHOW_LONG_NOFLUSH},
  {"Qcache_total_blocks",      (char*) &query_cache.total_blocks, SHOW_LONG_NOFLUSH},
#endif /*HAVE_QUERY_CACHE*/
  {"Queries",                  (char*) &show_queries,            SHOW_SIMPLE_FUNC},
  {"Questions",                (char*) offsetof(STATUS_VAR, questions), SHOW_LONG_STATUS},
#ifdef HAVE_REPLICATION
  {"Rpl_status",               (char*) &show_rpl_status,          SHOW_SIMPLE_FUNC},
#endif
  {"Select_full_join",         (char*) offsetof(STATUS_VAR, select_full_join_count_), SHOW_LONG_STATUS},
  {"Select_full_range_join",   (char*) offsetof(STATUS_VAR, select_full_range_join_count_), SHOW_LONG_STATUS},
  {"Select_range",             (char*) offsetof(STATUS_VAR, select_range_count_), SHOW_LONG_STATUS},
  {"Select_range_check",       (char*) offsetof(STATUS_VAR, select_range_check_count_), SHOW_LONG_STATUS},
  {"Select_scan",	       (char*) offsetof(STATUS_VAR, select_scan_count_), SHOW_LONG_STATUS},
  {"Slave_open_temp_tables",   (char*) &slave_open_temp_tables, SHOW_ATOMIC_COUNTER_UINT32_T},
#ifdef HAVE_REPLICATION
  {"Slaves_connected",        (char*) &binlog_dump_thread_count, SHOW_ATOMIC_COUNTER_UINT32_T},
  {"Slaves_running",          (char*) &show_slaves_running, SHOW_SIMPLE_FUNC },
  {"Slave_connections",       (char*) offsetof(STATUS_VAR, com_register_slave), SHOW_LONG_STATUS},
  {"Slave_heartbeat_period",   (char*) &show_heartbeat_period, SHOW_SIMPLE_FUNC},
  {"Slave_received_heartbeats",(char*) &show_slave_received_heartbeats, SHOW_SIMPLE_FUNC},
  {"Slave_retried_transactions",(char*)&slave_retried_transactions, SHOW_LONG},
  {"Slave_running",            (char*) &show_slave_running,     SHOW_SIMPLE_FUNC},
  {"Slave_skipped_errors",     (char*) &slave_skipped_errors, SHOW_LONGLONG},
#endif
  {"Slow_launch_threads",      (char*) &slow_launch_threads,    SHOW_LONG},
  {"Slow_queries",             (char*) offsetof(STATUS_VAR, long_query_count), SHOW_LONG_STATUS},
  {"Sort_merge_passes",	       (char*) offsetof(STATUS_VAR, filesort_merge_passes_), SHOW_LONG_STATUS},
  {"Sort_priority_queue_sorts",(char*) offsetof(STATUS_VAR, filesort_pq_sorts_), SHOW_LONG_STATUS}, 
  {"Sort_range",	       (char*) offsetof(STATUS_VAR, filesort_range_count_), SHOW_LONG_STATUS},
  {"Sort_rows",		       (char*) offsetof(STATUS_VAR, filesort_rows_), SHOW_LONG_STATUS},
  {"Sort_scan",		       (char*) offsetof(STATUS_VAR, filesort_scan_count_), SHOW_LONG_STATUS},
#ifdef HAVE_OPENSSL
#ifndef EMBEDDED_LIBRARY
  {"Ssl_accept_renegotiates",  (char*) &ssl_acceptor_stats.zero, SHOW_LONG},
  {"Ssl_accepts",              (char*) &ssl_acceptor_stats.accept, SHOW_LONG},
  {"Ssl_callback_cache_hits",  (char*) &ssl_acceptor_stats.zero, SHOW_LONG},
  {"Ssl_cipher",               (char*) &show_ssl_get_cipher, SHOW_SIMPLE_FUNC},
  {"Ssl_cipher_list",          (char*) &show_ssl_get_cipher_list, SHOW_SIMPLE_FUNC},
  {"Ssl_client_connects",      (char*) &ssl_acceptor_stats.zero, SHOW_LONG},
  {"Ssl_connect_renegotiates", (char*) &ssl_acceptor_stats.zero, SHOW_LONG},
  {"Ssl_ctx_verify_depth",     (char*) &ssl_acceptor_stats.verify_depth, SHOW_LONG},
  {"Ssl_ctx_verify_mode",      (char*) &ssl_acceptor_stats.verify_mode, SHOW_LONG},
  {"Ssl_default_timeout",      (char*) &show_ssl_get_default_timeout, SHOW_SIMPLE_FUNC},
  {"Ssl_finished_accepts",     (char*) &ssl_acceptor_stats.accept_good, SHOW_LONG},
  {"Ssl_finished_connects",    (char*) &ssl_acceptor_stats.zero, SHOW_LONG},
  {"Ssl_server_not_after",     (char*) &show_ssl_get_server_not_after, SHOW_SIMPLE_FUNC},
  {"Ssl_server_not_before",    (char*) &show_ssl_get_server_not_before, SHOW_SIMPLE_FUNC},
  {"Ssl_session_cache_hits",   (char*) &ssl_acceptor_stats.zero, SHOW_LONG},
  {"Ssl_session_cache_misses", (char*) &ssl_acceptor_stats.zero, SHOW_LONG},
  {"Ssl_session_cache_mode",   (char*) &ssl_acceptor_stats.session_cache_mode, SHOW_CHAR_PTR},
  {"Ssl_session_cache_overflows", (char*) &ssl_acceptor_stats.zero, SHOW_LONG},
  {"Ssl_session_cache_size",   (char*) &ssl_acceptor_stats.cache_size, SHOW_LONG},
  {"Ssl_session_cache_timeouts", (char*) &ssl_acceptor_stats.zero, SHOW_LONG},
  {"Ssl_sessions_reused",      (char*) &ssl_acceptor_stats.zero, SHOW_LONG},
  {"Ssl_used_session_cache_entries",(char*) &ssl_acceptor_stats.zero, SHOW_LONG},
  {"Ssl_verify_depth",         (char*) &show_ssl_get_verify_depth, SHOW_SIMPLE_FUNC},
  {"Ssl_verify_mode",          (char*) &show_ssl_get_verify_mode, SHOW_SIMPLE_FUNC},
  {"Ssl_version",              (char*) &show_ssl_get_version, SHOW_SIMPLE_FUNC},
#endif
#endif /* HAVE_OPENSSL */
  {"Syncs",                    (char*) &my_sync_count,          SHOW_LONG_NOFLUSH},
  /*
    Expression cache used only for caching subqueries now, so its statistic
    variables we call subquery_cache*.
  */
  {"Subquery_cache_hit",       (char*) &subquery_cache_hit,     SHOW_LONG},
  {"Subquery_cache_miss",      (char*) &subquery_cache_miss,    SHOW_LONG},
  {"Table_locks_immediate",    (char*) &locks_immediate,        SHOW_LONG},
  {"Table_locks_waited",       (char*) &locks_waited,           SHOW_LONG},
  {"Table_open_cache_active_instances", (char*) &show_tc_active_instances, SHOW_SIMPLE_FUNC},
  {"Table_open_cache_hits",    (char*) offsetof(STATUS_VAR, table_open_cache_hits), SHOW_LONGLONG_STATUS},
  {"Table_open_cache_misses",  (char*) offsetof(STATUS_VAR, table_open_cache_misses), SHOW_LONGLONG_STATUS},
  {"Table_open_cache_overflows", (char*) offsetof(STATUS_VAR, table_open_cache_overflows), SHOW_LONGLONG_STATUS},
#ifdef HAVE_MMAP
  {"Tc_log_max_pages_used",    (char*) &tc_log_max_pages_used,  SHOW_LONG},
  {"Tc_log_page_size",         (char*) &tc_log_page_size,       SHOW_LONG_NOFLUSH},
  {"Tc_log_page_waits",        (char*) &tc_log_page_waits,      SHOW_LONG},
#endif
#ifdef HAVE_POOL_OF_THREADS
  {"Threadpool_idle_threads",  (char *) &show_threadpool_idle_threads, SHOW_SIMPLE_FUNC},
  {"Threadpool_threads",       (char *) &show_threadpool_threads, SHOW_SIMPLE_FUNC},
#endif
  {"Threads_cached",           (char*) &show_cached_thread_count, SHOW_SIMPLE_FUNC},
  {"Threads_connected",        (char*) &connection_count,       SHOW_INT},
  {"Threads_created",	       (char*) &thread_created,		SHOW_LONG_NOFLUSH},
  {"Threads_running",          (char*) offsetof(STATUS_VAR, threads_running), SHOW_UINT32_STATUS},
  {"Transactions_multi_engine", (char*) &transactions_multi_engine, SHOW_LONG},
  {"Rpl_transactions_multi_engine", (char*) &rpl_transactions_multi_engine, SHOW_LONG},
  {"Transactions_gtid_foreign_engine", (char*) &transactions_gtid_foreign_engine, SHOW_LONG},
  {"Update_scan",	       (char*) offsetof(STATUS_VAR, update_scan_count), SHOW_LONG_STATUS},
  {"Uptime",                   (char*) &show_starttime,         SHOW_SIMPLE_FUNC},
#ifdef ENABLED_PROFILING
  {"Uptime_since_flush_status",(char*) &show_flushstatustime,   SHOW_SIMPLE_FUNC},
#endif
#ifdef WITH_WSREP
  {"wsrep_connected",         (char*) &wsrep_connected,         SHOW_BOOL},
  {"wsrep_ready",             (char*) &wsrep_show_ready,        SHOW_FUNC},
  {"wsrep_cluster_state_uuid",(char*) &wsrep_cluster_state_uuid,SHOW_CHAR_PTR},
  {"wsrep_cluster_conf_id",   (char*) &wsrep_cluster_conf_id,   SHOW_LONGLONG},
  {"wsrep_cluster_status",    (char*) &wsrep_cluster_status,    SHOW_CHAR_PTR},
  {"wsrep_cluster_size",      (char*) &wsrep_cluster_size,      SHOW_LONG_NOFLUSH},
  {"wsrep_local_index",       (char*) &wsrep_local_index,       SHOW_LONG_NOFLUSH},
  {"wsrep_local_bf_aborts",   (char*) &wsrep_show_bf_aborts,    SHOW_FUNC},
  {"wsrep_provider_name",     (char*) &wsrep_provider_name,     SHOW_CHAR_PTR},
  {"wsrep_provider_version",  (char*) &wsrep_provider_version,  SHOW_CHAR_PTR},
  {"wsrep_provider_vendor",   (char*) &wsrep_provider_vendor,   SHOW_CHAR_PTR},
  {"wsrep_provider_capabilities", (char*) &wsrep_provider_capabilities, SHOW_CHAR_PTR},
  {"wsrep_thread_count",      (char*) &wsrep_running_threads,   SHOW_LONG_NOFLUSH},
  {"wsrep_applier_thread_count", (char*) &wsrep_running_applier_threads, SHOW_LONG_NOFLUSH},
  {"wsrep_rollbacker_thread_count", (char *) &wsrep_running_rollbacker_threads, SHOW_LONG_NOFLUSH},
  {"wsrep_cluster_capabilities", (char*) &wsrep_cluster_capabilities, SHOW_CHAR_PTR},
  {"wsrep",                    (char*) &wsrep_show_status,       SHOW_FUNC},
#endif
  {NullS, NullS, SHOW_LONG}
};

static bool add_terminator(DYNAMIC_ARRAY *options)
{
  my_option empty_element= {0, 0, 0, 0, 0, 0, GET_NO_ARG, NO_ARG, 0, 0, 0, 0, 0, 0};
  return insert_dynamic(options, (uchar *)&empty_element);
}

static bool add_many_options(DYNAMIC_ARRAY *options, my_option *list,
                            size_t elements)
{
  for (my_option *opt= list; opt < list + elements; opt++)
    if (insert_dynamic(options, opt))
      return 1;
  return 0;
}

#ifndef EMBEDDED_LIBRARY
static void print_version(void)
{
  if (IS_SYSVAR_AUTOSIZE(&server_version_ptr))
    set_server_version(server_version, sizeof(server_version));

  printf("%s  Ver %s for %s on %s (%s)\n",my_progname,
	 server_version,SYSTEM_TYPE,MACHINE_TYPE, MYSQL_COMPILATION_COMMENT);
}

/** Compares two options' names, treats - and _ the same */
static int option_cmp(my_option *a, my_option *b)
{
  const char *sa= a->name;
  const char *sb= b->name;
  for (; *sa || *sb; sa++, sb++)
  {
    if (*sa < *sb)
    {
      if (*sa == '-' && *sb == '_')
        continue;
      else
        return -1;
    }
    if (*sa > *sb)
    {
      if (*sa == '_' && *sb == '-')
        continue;
      else
        return 1;
    }
  }
  return 0;
}

static void print_help()
{
  MEM_ROOT mem_root;
  init_alloc_root(PSI_NOT_INSTRUMENTED, &mem_root, 4096, 4096, MYF(0));

  pop_dynamic(&all_options);
  add_many_options(&all_options, pfs_early_options,
                  array_elements(pfs_early_options));
  sys_var_add_options(&all_options, sys_var::PARSE_EARLY);
  add_plugin_options(&all_options, &mem_root);
  sort_dynamic(&all_options, (qsort_cmp) option_cmp);
  sort_dynamic(&all_options, (qsort_cmp) option_cmp);
  add_terminator(&all_options);

  my_print_help((my_option*) all_options.buffer);

  /* Add variables that must be shown but not changed, like version numbers */
  pop_dynamic(&all_options);
  sys_var_add_options(&all_options, sys_var::GETOPT_ONLY_HELP);
  sort_dynamic(&all_options, (qsort_cmp) option_cmp);
  add_terminator(&all_options);
  my_print_variables((my_option*) all_options.buffer);

  free_root(&mem_root, MYF(0));
}

static void usage(void)
{
  DBUG_ENTER("usage");
  myf utf8_flag= global_system_variables.old_behavior &
                 OLD_MODE_UTF8_IS_UTF8MB3 ? MY_UTF8_IS_UTF8MB3 : 0;
  if (!(default_charset_info= get_charset_by_csname(default_character_set_name,
					           MY_CS_PRIMARY,
						         MYF(utf8_flag | MY_WME))))
    exit(1);
  if (!default_collation_name)
    default_collation_name= (char*) default_charset_info->coll_name.str;
  print_version();
  puts(ORACLE_WELCOME_COPYRIGHT_NOTICE("2000"));
  puts("Starts the MariaDB database server.\n");
  printf("Usage: %s [OPTIONS]\n", my_progname);
  if (!opt_verbose)
    puts("\nFor more help options (several pages), use mysqld --verbose --help.");
  else
  {
#ifdef _WIN32
  puts("NT and Win32 specific options:\n"
       "  --install                     Install the default service (NT).\n"
       "  --install-manual              Install the default service started manually (NT).\n"
       "  --install service_name        Install an optional service (NT).\n"
       "  --install-manual service_name Install an optional service started manually (NT).\n"
       "  --remove                      Remove the default service from the service list (NT).\n"
       "  --remove service_name         Remove the service_name from the service list (NT).\n"
       "  --enable-named-pipe           Only to be used for the default server (NT).\n"
       "  --standalone                  Dummy option to start as a standalone server (NT).");
  puts("");
#endif
  print_defaults(MYSQL_CONFIG_NAME,load_default_groups);
  puts("");
  set_ports();

  /* Print out all the options including plugin supplied options */
  print_help();

  if (! plugins_are_initialized)
  {
    puts("\nPlugins have parameters that are not reflected in this list"
         "\nbecause execution stopped before plugins were initialized.");
  }

    puts("\nTo see what variables a running server is using, type"
         "\n'SELECT * FROM INFORMATION_SCHEMA.GLOBAL_VARIABLES' instead of 'mysqld --verbose --help' or 'mariadbd --verbose --help'.");
  }
  DBUG_VOID_RETURN;
}
#endif /*!EMBEDDED_LIBRARY*/

/**
  Initialize MySQL global variables to default values.

  @note
    The reason to set a lot of global variables to zero is to allow one to
    restart the embedded server with a clean environment
    It's also needed on some exotic platforms where global variables are
    not set to 0 when a program starts.

    We don't need to set variables referred to in my_long_options
    as these are initialized by my_getopt.
*/

static int mysql_init_variables(void)
{
  /* Things reset to zero */
  opt_skip_slave_start= opt_reckless_slave = 0;
  mysql_home[0]= pidfile_name[0]= log_error_file[0]= 0;
#if defined(HAVE_REALPATH) && !defined(HAVE_valgrind) && !defined(HAVE_BROKEN_REALPATH)
  /*  We can only test for sub paths if my_symlink.c is using realpath */
  mysys_test_invalid_symlink= path_starts_from_data_home_dir;
#endif
  opt_log= 0;
  opt_bin_log= opt_bin_log_used= 0;
  opt_disable_networking= opt_skip_show_db=0;
  opt_skip_name_resolve= 0;
  opt_ignore_builtin_innodb= 0;
  opt_logname= opt_binlog_index_name= opt_slow_logname= 0;
  opt_log_basename= 0;
  opt_tc_log_file= (char *)"tc.log";      // no hostname in tc_log file name !
  opt_ddl_recovery_file= (char *) "ddl_recovery.log";
  opt_secure_auth= 0;
  opt_bootstrap= opt_myisam_log= 0;
  disable_log_notes= 0;
  mqh_used= 0;
  cleanup_done= 0;
  select_errors= dropping_tables= ha_open_options=0;
  THD_count::count= CONNECT::count= 0;
  slave_open_temp_tables= 0;
  opt_endinfo= using_udf_functions= 0;
  opt_using_transactions= 0;
  abort_loop= select_thread_in_use= signal_thread_in_use= 0;
  grant_option= 0;
  aborted_threads= aborted_connects= aborted_connects_preauth= 0;
  malloc_calls= 0;
  subquery_cache_miss= subquery_cache_hit= 0;
  delayed_insert_threads= delayed_insert_writes= delayed_rows_in_use= 0;
  delayed_insert_errors= thread_created= 0;
  specialflag= 0;
  binlog_cache_use=  binlog_cache_disk_use= 0;
  max_used_connections= slow_launch_threads = 0;
  mysqld_user= mysqld_chroot= opt_init_file= opt_bin_logname = 0;
  prepared_stmt_count= 0;
  mysqld_unix_port= opt_mysql_tmpdir= my_bind_addr_str= NullS;
  bzero((uchar*) &mysql_tmpdir_list, sizeof(mysql_tmpdir_list));
  /* Clear all except global_memory_used */
  bzero((char*) &global_status_var, offsetof(STATUS_VAR,
                                             last_cleared_system_status_var));
  opt_large_pages= 0;
  opt_super_large_pages= 0;
#if defined(ENABLED_DEBUG_SYNC)
  opt_debug_sync_timeout= 0;
#endif /* defined(ENABLED_DEBUG_SYNC) */
  key_map_full.set_all();

  /* Character sets */
  system_charset_info= &my_charset_utf8mb3_general_ci;
  files_charset_info= &my_charset_utf8mb3_general_ci;
  national_charset_info= &my_charset_utf8mb3_general_ci;
  table_alias_charset= &my_charset_bin;
  character_set_filesystem= &my_charset_bin;

  opt_specialflag= SPECIAL_ENGLISH;
  mysql_home_ptr= mysql_home;
  log_error_file_ptr= log_error_file;
  protocol_version= PROTOCOL_VERSION;
  what_to_log= ~(1UL << COM_TIME);
  denied_connections= 0;
  executed_events= 0;
  global_query_id= 1;
  global_thread_id= 0;
  strnmov(server_version, MYSQL_SERVER_VERSION, sizeof(server_version)-1);
  thread_cache.init();
  key_caches.empty();
  if (!(dflt_key_cache= get_or_create_key_cache(default_key_cache_base.str,
                                                default_key_cache_base.length)))
  {
    sql_print_error("Cannot allocate the keycache");
    return 1;
  }

  /* set key_cache_hash.default_value = dflt_key_cache */
  multi_keycache_init();

  /* Set directory paths */
  mysql_real_data_home_len=
    (uint)(strmake_buf(mysql_real_data_home,
                get_relative_path(MYSQL_DATADIR)) - mysql_real_data_home);
  /* Replication parameters */
  master_info_file= (char*) "master.info",
    relay_log_info_file= (char*) "relay-log.info";
  report_user= report_password = report_host= 0;	/* TO BE DELETED */
  opt_relay_logname= opt_relaylog_index_name= 0;
  slave_retried_transactions= 0;
  transactions_multi_engine= 0;
  rpl_transactions_multi_engine= 0;
  transactions_gtid_foreign_engine= 0;
  log_bin_basename= NULL;
  log_bin_index= NULL;

  /* Variables in libraries */
  charsets_dir= 0;
  default_character_set_name= (char*) MYSQL_DEFAULT_CHARSET_NAME;
  default_collation_name= compiled_default_collation_name;
  character_set_filesystem_name= (char*) "binary";
  lc_messages= (char*) "en_US";
  lc_time_names_name= (char*) "en_US";
  
  /* Variables that depends on compile options */
#ifndef DBUG_OFF
  default_dbug_option=IF_WIN("d:t:i:O,\\mariadbd.trace",
			     "d:t:i:o,/tmp/mariadbd.trace");
  current_dbug_option= default_dbug_option;
#endif
  opt_error_log= IF_WIN(1,0);
#ifdef ENABLED_PROFILING
    have_profiling = SHOW_OPTION_YES;
#else
    have_profiling = SHOW_OPTION_NO;
#endif

#if defined(HAVE_OPENSSL) && !defined(EMBEDDED_LIBRARY)
  have_ssl=SHOW_OPTION_YES;
#if defined(HAVE_WOLFSSL)
  have_openssl= SHOW_OPTION_NO;
#else
  have_openssl= SHOW_OPTION_YES;
#endif
#else
  have_openssl= have_ssl= SHOW_OPTION_NO;
#endif
#ifdef HAVE_BROKEN_REALPATH
  have_symlink=SHOW_OPTION_NO;
#else
  have_symlink=SHOW_OPTION_YES;
#endif
#ifdef HAVE_DLOPEN
  have_dlopen=SHOW_OPTION_YES;
#else
  have_dlopen=SHOW_OPTION_NO;
#endif
#ifdef HAVE_QUERY_CACHE
  have_query_cache=SHOW_OPTION_YES;
#else
  have_query_cache=SHOW_OPTION_NO;
#endif
#ifdef HAVE_SPATIAL
  have_geometry=SHOW_OPTION_YES;
#else
  have_geometry=SHOW_OPTION_NO;
#endif
#ifdef HAVE_RTREE_KEYS
  have_rtree_keys=SHOW_OPTION_YES;
#else
  have_rtree_keys=SHOW_OPTION_NO;
#endif
#ifdef HAVE_CRYPT
  have_crypt=SHOW_OPTION_YES;
#else
  have_crypt=SHOW_OPTION_NO;
#endif
#ifdef HAVE_COMPRESS
  have_compress= SHOW_OPTION_YES;
#else
  have_compress= SHOW_OPTION_NO;
#endif
#ifdef HAVE_LIBWRAP
  libwrapName= NullS;
#endif
#ifdef HAVE_OPENSSL
  des_key_file = 0;
#ifndef EMBEDDED_LIBRARY
  ssl_acceptor_fd= 0;
#endif /* ! EMBEDDED_LIBRARY */
#endif /* HAVE_OPENSSL */

#if defined(_WIN32)
  /* Allow Win32 users to move MySQL anywhere */
  {
    char prg_dev[LIBLEN];
    char executing_path_name[LIBLEN];
    if (!test_if_hard_path(my_progname))
    {
      // we don't want to use GetModuleFileName inside of my_path since
      // my_path is a generic path dereferencing function and here we care
      // only about the executing binary.
      GetModuleFileName(NULL, executing_path_name, sizeof(executing_path_name));
      my_path(prg_dev, executing_path_name, NULL);
    }
    else
      my_path(prg_dev, my_progname, "mysql/bin");
    strcat(prg_dev,"/../");			// Remove 'bin' to get base dir
    cleanup_dirname(mysql_home,prg_dev);
  }
#else
  const char *tmpenv;
  if (!(tmpenv = getenv("MY_BASEDIR_VERSION")))
    tmpenv = DEFAULT_MYSQL_HOME;
  strmake_buf(mysql_home, tmpenv);
  set_sys_var_value_origin(&mysql_home_ptr, sys_var::ENV);
#endif

  if (wsrep_init_vars())
    return 1;

  return 0;
}

my_bool
mysqld_get_one_option(const struct my_option *opt, const char *argument,
                      const char *filename)
{
  if (opt->app_type)
  {
    sys_var *var= (sys_var*) opt->app_type;
    if (argument == autoset_my_option)
    {
      var->value_origin= sys_var::AUTO;
      return 0;
    }
    if (*filename)
    {
      var->origin_filename= filename;
      var->value_origin= sys_var::CONFIG;
    }
    else
      var->value_origin= sys_var::COMMAND_LINE;
  }

  switch(opt->id) {
  case '#':
#ifndef DBUG_OFF
    if (!argument)
      argument= (char*) default_dbug_option;
    if (argument[0] == '0' && !argument[1])
    {
      DEBUGGER_OFF;
      break;
    }
    DEBUGGER_ON;
    if (argument[0] == '1' && !argument[1])
      break;
    DBUG_SET_INITIAL(argument);
    current_dbug_option= argument;
    opt_endinfo=1;				/* unireg: memory allocation */
#else
    sql_print_warning("'%s' is disabled in this build", opt->name);
#endif
    break;
  case OPT_REMOVED_OPTION:
    sql_print_warning("'%s' was removed. It does nothing now and exists only "
                      "for compatibility with old my.cnf files.", opt->name);
    break;
  case OPT_MYSQL_COMPATIBILITY:
    sql_print_warning("'%s' is MySQL 5.6 / 5.7 compatible option. Not used or "
                      "needed in MariaDB.", opt->name);
    break;
  case OPT_MYSQL_TO_BE_IMPLEMENTED:
    sql_print_warning("'%s' is MySQL 5.6 / 5.7 compatible option. To be "
                      "implemented in later versions.", opt->name);
    break;
  case 'a':
    SYSVAR_AUTOSIZE(global_system_variables.sql_mode, MODE_ANSI);
    SYSVAR_AUTOSIZE(global_system_variables.tx_isolation, ISO_SERIALIZABLE);
    break;
  case 'b':
    strmake_buf(mysql_home, argument);
    break;
  case 'C':
    if (default_collation_name == compiled_default_collation_name)
      default_collation_name= 0;
    break;
  case 'h':
    strmake_buf(mysql_real_data_home, argument);
    /* Correct pointer set by my_getopt (for embedded library) */
    mysql_real_data_home_ptr= mysql_real_data_home;
    break;
  case 'u':
    if (!mysqld_user || !strcmp(mysqld_user, argument))
      mysqld_user= argument;
    else
      sql_print_warning("Ignoring user change to '%s' because the user was set to '%s' earlier on the command line\n", argument, mysqld_user);
    break;
  case 'L':
    strmake_buf(lc_messages_dir, argument);
    break;
  case OPT_BINLOG_FORMAT:
    binlog_format_used= true;
    break;
#include <sslopt-case.h>
  case 'V':
    if (argument)
    {
      strmake(server_version, argument, sizeof(server_version) - 1);
      set_sys_var_value_origin(&server_version_ptr,
                *filename ? sys_var::CONFIG : sys_var::COMMAND_LINE, filename);
      using_custom_server_version= true;
    }
#ifndef EMBEDDED_LIBRARY
    else
    {
      print_version();
      opt_abort= 1;                    // Abort after parsing all options
    }
#endif /*EMBEDDED_LIBRARY*/
    break;
  case 'W':
    if (!argument)
      global_system_variables.log_warnings++;
    else if (argument == disabled_my_option)
      global_system_variables.log_warnings= 0L;
    else
      global_system_variables.log_warnings= atoi(argument);
    break;
  case 'T':
    test_flags= argument ? (uint) atoi(argument) : 0;
    opt_endinfo=1;
    break;
  case OPT_THREAD_CONCURRENCY:
    WARN_DEPRECATED_NO_REPLACEMENT(NULL, "THREAD_CONCURRENCY");
    break;
  case (int) OPT_ISAM_LOG:
    opt_myisam_log=1;
    break;
  case (int) OPT_BIN_LOG:
    opt_bin_log= MY_TEST(argument != disabled_my_option);
    opt_bin_log_used= 1;
    break;
  case (int) OPT_LOG_BASENAME:
  {
    if (opt_log_basename[0] == 0 || strchr(opt_log_basename, FN_EXTCHAR) ||
        strchr(opt_log_basename,FN_LIBCHAR) ||
        !is_filename_allowed(opt_log_basename, strlen(opt_log_basename), FALSE))
    {
      sql_print_error("Wrong argument for --log-basename. It can't be empty or contain '.' or '" FN_DIRSEP "'. It must be valid filename.");
      return 1;
    }
    if (log_error_file_ptr != disabled_my_option)
      SYSVAR_AUTOSIZE(log_error_file_ptr, opt_log_basename);

    /* General log file */
    make_default_log_name(&opt_logname, ".log", false);
    /* Slow query log file */
    make_default_log_name(&opt_slow_logname, "-slow.log", false);
    /* Binary log file */
    make_default_log_name(&opt_bin_logname, "-bin", true);
    /* Binary log index file */
    make_default_log_name(&opt_binlog_index_name, "-bin.index", true);
    set_sys_var_value_origin(&opt_logname, sys_var::AUTO);
    set_sys_var_value_origin(&opt_slow_logname, sys_var::AUTO);
    if (!opt_logname || !opt_slow_logname || !opt_bin_logname ||
        !opt_binlog_index_name)
      return 1;

#ifdef HAVE_REPLICATION
    /* Relay log file */
    make_default_log_name(&opt_relay_logname, "-relay-bin", true);
    /* Relay log index file */
    make_default_log_name(&opt_relaylog_index_name, "-relay-bin.index", true);
    set_sys_var_value_origin(&opt_relay_logname, sys_var::AUTO);
    if (!opt_relay_logname || !opt_relaylog_index_name)
      return 1;
#endif

    if (IS_SYSVAR_AUTOSIZE(&pidfile_name_ptr))
    {
      SYSVAR_AUTOSIZE(pidfile_name_ptr, pidfile_name);
      /* PID file */
      strmake(pidfile_name, argument, sizeof(pidfile_name)-5);
      strmov(fn_ext(pidfile_name),".pid");
    }
    break;
  }
  case (int)OPT_EXPIRE_LOGS_DAYS:
  {
    binlog_expire_logs_seconds= (ulong)(expire_logs_days*24*60*60);
    break;
  }
  case (int)OPT_BINLOG_EXPIRE_LOGS_SECONDS:
  {
    expire_logs_days= (binlog_expire_logs_seconds/double (24*60*60));
    break;
  }

#ifdef HAVE_REPLICATION
  case (int)OPT_REPLICATE_IGNORE_DB:
  {
    cur_rpl_filter->add_ignore_db(argument);
    break;
  }
  case (int)OPT_REPLICATE_DO_DB:
  {
    cur_rpl_filter->add_do_db(argument);
    break;
  }
  case (int)OPT_REPLICATE_REWRITE_DB:
  {
    /* See also OPT_REWRITE_DB handling in client/mysqlbinlog.cc */
    const char* key= argument, *ptr, *val;

    // Skipp pre-space in key
    while (*key && my_isspace(mysqld_charset, *key))
      key++;

    // Where val begins
    if (!(ptr= strstr(key, "->")))
    {
      sql_print_error("Bad syntax in replicate-rewrite-db: missing '->'");
      return 1;
    }
    val= ptr+2;

    // Skip blanks at the end of key
    while (ptr > key && my_isspace(mysqld_charset, ptr[-1]))
      ptr--;
    if (ptr == key)
    {
      sql_print_error("Bad syntax in replicate-rewrite-db - empty FROM db");
      return 1;
    }
    key= strmake_root(&startup_root, key, (size_t) (ptr-key));

    /* Skipp pre space in value */
    while (*val && my_isspace(mysqld_charset, *val))
      val++;

    // Value ends with \0 or space
    for (ptr= val; *ptr && !my_isspace(&my_charset_latin1, *ptr) ; ptr++)
    {}
    if (ptr == val)
    {
      sql_print_error("Bad syntax in replicate-rewrite-db - empty TO db");
      return 1;
    }
    val= strmake_root(&startup_root, val, (size_t) (ptr-val));

    cur_rpl_filter->add_db_rewrite(key, val);
    break;
  }
  case (int)OPT_SLAVE_PARALLEL_MODE:
  {
    /* Store latest mode for Master::Info */
    cur_rpl_filter->set_parallel_mode
      ((enum_slave_parallel_mode)opt_slave_parallel_mode);
    break;
  }

  case (int)OPT_BINLOG_IGNORE_DB:
  {
    binlog_filter->add_ignore_db(argument);
    break;
  }
  case (int)OPT_BINLOG_DO_DB:
  {
    binlog_filter->add_do_db(argument);
    break;
  }
  case (int)OPT_REPLICATE_DO_TABLE:
  {
    if (cur_rpl_filter->add_do_table(argument))
    {
      sql_print_error("Could not add do table rule '%s'", argument);
      return 1;
    }
    break;
  }
  case (int)OPT_REPLICATE_WILD_DO_TABLE:
  {
    if (cur_rpl_filter->add_wild_do_table(argument))
    {
      sql_print_error("Could not add do table rule '%s'", argument);
      return 1;
    }
    break;
  }
  case (int)OPT_REPLICATE_WILD_IGNORE_TABLE:
  {
    if (cur_rpl_filter->add_wild_ignore_table(argument))
    {
      sql_print_error("Could not add ignore table rule '%s'", argument);
      return 1;
    }
    break;
  }
  case (int)OPT_REPLICATE_IGNORE_TABLE:
  {
    if (cur_rpl_filter->add_ignore_table(argument))
    {
      sql_print_error("Could not add ignore table rule '%s'", argument);
      return 1;
    }
    break;
  }
#endif /* HAVE_REPLICATION */
  case (int) OPT_SAFE:
    opt_specialflag|= SPECIAL_SAFE_MODE | SPECIAL_NO_NEW_FUNC;
    SYSVAR_AUTOSIZE(delay_key_write_options, (uint) DELAY_KEY_WRITE_NONE);
    SYSVAR_AUTOSIZE(myisam_recover_options, HA_RECOVER_DEFAULT);
    ha_open_options&= ~(HA_OPEN_DELAY_KEY_WRITE);
#ifdef HAVE_QUERY_CACHE
    SYSVAR_AUTOSIZE(query_cache_size, 0);
#endif
    sql_print_warning("The syntax '--safe-mode' is deprecated and will be "
                      "removed in a future release.");
    break;
  case (int) OPT_SKIP_HOST_CACHE:
    opt_specialflag|= SPECIAL_NO_HOST_CACHE;
    break;
  case (int) OPT_SKIP_RESOLVE:
    if ((opt_skip_name_resolve= (argument != disabled_my_option)))
      opt_specialflag|= SPECIAL_NO_RESOLVE;
    else
      opt_specialflag&= ~SPECIAL_NO_RESOLVE;
    break;
  case (int) OPT_WANT_CORE:
    test_flags |= TEST_CORE_ON_SIGNAL;
    break;
  case OPT_CONSOLE:
    if (opt_console)
      opt_error_log= 0;			// Force logs to stdout
    break;
  case OPT_BOOTSTRAP:
    opt_noacl=opt_bootstrap=1;
#ifdef _WIN32
    {
      /*
       Check if security descriptor is passed from
       mysql_install_db.exe.
       Used by Windows installer to correctly setup
       privileges on the new directories.
      */
      char* dir_sddl = getenv("MARIADB_NEW_DIRECTORY_SDDL");
      if (dir_sddl)
      {
        ConvertStringSecurityDescriptorToSecurityDescriptor(
          dir_sddl, SDDL_REVISION_1, &my_dir_security_attributes.lpSecurityDescriptor, NULL);
        DBUG_ASSERT(my_dir_security_attributes.lpSecurityDescriptor);
      }
    }
#endif
    break;
  case OPT_SERVER_ID:
    ::server_id= global_system_variables.server_id;
    break;
  case OPT_LOWER_CASE_TABLE_NAMES:
    lower_case_table_names_used= 1;
    break;
#if defined(ENABLED_DEBUG_SYNC)
  case OPT_DEBUG_SYNC_TIMEOUT:
    /*
      Debug Sync Facility. See debug_sync.cc.
      Default timeout for WAIT_FOR action.
      Default value is zero (facility disabled).
      If option is given without an argument, supply a non-zero value.
    */
    if (!argument)
    {
      /* purecov: begin tested */
      opt_debug_sync_timeout= DEBUG_SYNC_DEFAULT_WAIT_TIMEOUT;
      /* purecov: end */
    }
    break;
#endif /* defined(ENABLED_DEBUG_SYNC) */
  case OPT_LOG_ERROR:
    /*
      "No --log-error" == "write errors to stderr",
      "--log-error without argument" == "write errors to a file".
    */
    if (argument == NULL) /* no argument */
      log_error_file_ptr= const_cast<char*>("");
    break;
  case OPT_IGNORE_DB_DIRECTORY:
    opt_ignore_db_dirs= NULL; // will be set in ignore_db_dirs_process_additions
    if (*argument == 0)
      ignore_db_dirs_reset();
    else
    {
      if (push_ignored_db_dir(argument))
      {
        sql_print_error("Can't start server: "
                        "cannot process --ignore-db-dir=%.*s", 
                        FN_REFLEN, argument);
        return 1;
      }
    }
    break;
  case OPT_PLUGIN_LOAD:
    free_list(opt_plugin_load_list_ptr);
    if (argument == disabled_my_option)
      break;                                    // Resets plugin list
    /* fall through */
  case OPT_PLUGIN_LOAD_ADD:
    opt_plugin_load_list_ptr->push_back(new i_string(argument));
    break;
  case OPT_PFS_INSTRUMENT:
  {
#ifdef WITH_PERFSCHEMA_STORAGE_ENGINE
#ifndef EMBEDDED_LIBRARY
    /* Parse instrument name and value from argument string */
    const char *name= argument, *ptr, *val;

    /* Trim leading spaces from instrument name */
    while (*name && my_isspace(mysqld_charset, *name))
      name++;

    /* Assignment required */
    if (!(ptr= strchr(name, '=')))
    {
       my_getopt_error_reporter(WARNING_LEVEL,
                             "Missing value for performance_schema_instrument "
                             "'%s'", argument);
      return 0;
    }

    /* Option value */
    val= ptr + 1;

    /* Trim trailing spaces and slashes from instrument name */
    while (ptr > name && (my_isspace(mysqld_charset, ptr[-1]) ||
                          ptr[-1] == '/'))
      ptr--;
    if (ptr == name)
    {
       my_getopt_error_reporter(WARNING_LEVEL,
                             "Invalid instrument name for "
                             "performance_schema_instrument '%s'", name);
       return 0;
    }
    name= strmake_root(&startup_root, name, (size_t) (ptr - name));

    /* Trim leading spaces from option value */
    while (*val && my_isspace(mysqld_charset, *val))
      val++;

    /* Find end of value */
    for (ptr= val; *ptr && !my_isspace(mysqld_charset, *ptr) ; ptr++)
    {}
    if (ptr == val)
    {
       my_getopt_error_reporter(WARNING_LEVEL,
                             "No value for performance_schema_instrument "
                             "'%s'", name);
      return 0;
    }
    val= strmake_root(&startup_root, val, (size_t) (ptr - val));

    /* Add instrument name and value to array of configuration options */
    if (add_pfs_instr_to_array(name, val))
    {
       my_getopt_error_reporter(WARNING_LEVEL,
                             "Invalid value for performance_schema_instrument "
                             "'%s'", name);
      return 0;
    }
#endif /* EMBEDDED_LIBRARY */
#endif
    break;
  }
#ifdef WITH_WSREP
  case OPT_WSREP_CAUSAL_READS:
  {
    if (global_system_variables.wsrep_causal_reads)
    {
      WSREP_WARN("option --wsrep-causal-reads is deprecated");
      if (!(global_system_variables.wsrep_sync_wait & WSREP_SYNC_WAIT_BEFORE_READ))
      {
        WSREP_WARN("--wsrep-causal-reads=ON takes precedence over --wsrep-sync-wait=%u. "
                     "WSREP_SYNC_WAIT_BEFORE_READ is on",
                     global_system_variables.wsrep_sync_wait);
        global_system_variables.wsrep_sync_wait |= WSREP_SYNC_WAIT_BEFORE_READ;
      }
    }
    else
    {
      if (global_system_variables.wsrep_sync_wait & WSREP_SYNC_WAIT_BEFORE_READ) {
          WSREP_WARN("--wsrep-sync-wait=%u takes precedence over --wsrep-causal-reads=OFF. "
                     "WSREP_SYNC_WAIT_BEFORE_READ is on",
                     global_system_variables.wsrep_sync_wait);
          global_system_variables.wsrep_causal_reads = 1;
      }
    }
    break;
  }
  case OPT_WSREP_SYNC_WAIT:
    global_system_variables.wsrep_causal_reads=
      MY_TEST(global_system_variables.wsrep_sync_wait &
              WSREP_SYNC_WAIT_BEFORE_READ);
    break;
#endif /* WITH_WSREP */
  }
  return 0;
}


/** Handle arguments for multiple key caches. */

C_MODE_START

static void*
mysql_getopt_value(const char *name, uint length,
		   const struct my_option *option, int *error)
{
  if (error)
    *error= 0;
  switch (option->id) {
  case OPT_KEY_BUFFER_SIZE:
  case OPT_KEY_CACHE_BLOCK_SIZE:
  case OPT_KEY_CACHE_DIVISION_LIMIT:
  case OPT_KEY_CACHE_AGE_THRESHOLD:
  case OPT_KEY_CACHE_PARTITIONS:
  case OPT_KEY_CACHE_CHANGED_BLOCKS_HASH_SIZE:
  {
    KEY_CACHE *key_cache;
    if (unlikely(!(key_cache= get_or_create_key_cache(name, length))))
    {
      if (error)
        *error= EXIT_OUT_OF_MEMORY;
      return 0;
    }
    switch (option->id) {
    case OPT_KEY_BUFFER_SIZE:
      return &key_cache->param_buff_size;
    case OPT_KEY_CACHE_BLOCK_SIZE:
      return &key_cache->param_block_size;
    case OPT_KEY_CACHE_DIVISION_LIMIT:
      return &key_cache->param_division_limit;
    case OPT_KEY_CACHE_AGE_THRESHOLD:
      return &key_cache->param_age_threshold;
    case OPT_KEY_CACHE_PARTITIONS:
      return (uchar**) &key_cache->param_partitions;
    case OPT_KEY_CACHE_CHANGED_BLOCKS_HASH_SIZE:
      return (uchar**) &key_cache->changed_blocks_hash_size;
    }
  }
  /* We return in all cases above. Let us silence -Wimplicit-fallthrough */
  DBUG_ASSERT(0);
#ifdef HAVE_REPLICATION
  /* fall through */
  case OPT_REPLICATE_DO_DB:
  case OPT_REPLICATE_DO_TABLE:
  case OPT_REPLICATE_IGNORE_DB:
  case OPT_REPLICATE_IGNORE_TABLE:
  case OPT_REPLICATE_WILD_DO_TABLE:
  case OPT_REPLICATE_WILD_IGNORE_TABLE:
  case OPT_REPLICATE_REWRITE_DB:
  case OPT_SLAVE_PARALLEL_MODE:
  {
    /* Store current filter for mysqld_get_one_option() */
    if (!(cur_rpl_filter= get_or_create_rpl_filter(name, length)))
    {
      if (error)
        *error= EXIT_OUT_OF_MEMORY;
    }
    if (option->id == OPT_SLAVE_PARALLEL_MODE)
    {
      /*
        Ensure parallel_mode variable is shown in --help. The other
        variables are not easily printable here.
       */
      return (char**) &opt_slave_parallel_mode;
    }
    return 0;
  }
#endif
  }
  return option->value;
}

static void option_error_reporter(enum loglevel level, const char *format, ...)
{
  va_list args;
  va_start(args, format);

  /*
    Don't print warnings for --loose options during bootstrap if
    log_warnings <= 2 (2 is default) as warnings during bootstrap
    can confuse people when running mysql_install_db and other scripts.
    Don't print loose warnings at all if log_warnings <= 1
  */
  if (level == ERROR_LEVEL ||
      (global_system_variables.log_warnings >
       (ulong) (1 + MY_TEST(opt_bootstrap))))
  {
    vprint_msg_to_log(level, format, args);
  }
  va_end(args);
}

C_MODE_END

/**
  Get server options from the command line,
  and perform related server initializations.
  @param [in, out] argc_ptr       command line options (count)
  @param [in, out] argv_ptr       command line options (values)
  @return 0 on success

  @todo
  - FIXME add EXIT_TOO_MANY_ARGUMENTS to "mysys_err.h" and return that code?
*/
static int get_options(int *argc_ptr, char ***argv_ptr)
{
  int ho_error;

  my_getopt_get_addr= mysql_getopt_value;
  my_getopt_error_reporter= option_error_reporter;

  /* prepare all_options array */
  my_init_dynamic_array(PSI_INSTRUMENT_ME, &all_options, sizeof(my_option),
                        array_elements(my_long_options) + sys_var_elements(),
                        array_elements(my_long_options)/4, MYF(0));
  add_many_options(&all_options, my_long_options, array_elements(my_long_options));
  sys_var_add_options(&all_options, 0);
  add_terminator(&all_options);

  /* Skip unknown options so that they may be processed later by plugins */
  my_getopt_skip_unknown= TRUE;

  if ((ho_error= handle_options(argc_ptr, argv_ptr,
                                (my_option*) (all_options.buffer),
                                mysqld_get_one_option)))
    return ho_error;

  if (!opt_help)
    delete_dynamic(&all_options);
  else
    opt_abort= 1;

  /* Add back the program name handle_options removes */
  (*argc_ptr)++;
  (*argv_ptr)--;

  disable_log_notes= opt_silent_startup;

  /*
    Options have been parsed. Now some of them need additional special
    handling, like custom value checking, checking of incompatibilites
    between options, setting of multiple variables, etc.
    Do them here.
  */

  if (global_system_variables.old_mode)
  {
    global_system_variables.old_behavior|= (OLD_MODE_NO_PROGRESS_INFO |
                                           OLD_MODE_IGNORE_INDEX_ONLY_FOR_JOIN |
                                           OLD_MODE_COMPAT_5_1_CHECKSUM);
    sql_print_warning("--old is deprecated and will be removed in a future "
                      "release. Please use --old-mode instead. ");
  }

  if (global_system_variables.net_buffer_length > 
      global_system_variables.max_allowed_packet)
  {
    sql_print_warning("net_buffer_length (%lu) is set to be larger "
                      "than max_allowed_packet (%lu). Please rectify.",
                      global_system_variables.net_buffer_length, 
                      global_system_variables.max_allowed_packet);
  }

  if (log_error_file_ptr != disabled_my_option)
    opt_error_log= 1;
  else
    log_error_file_ptr= const_cast<char*>("");

  opt_init_connect.length=strlen(opt_init_connect.str);
  opt_init_slave.length=strlen(opt_init_slave.str);

  if (global_system_variables.low_priority_updates)
    thr_upgraded_concurrent_insert_lock= TL_WRITE_LOW_PRIORITY;

  if (ft_boolean_check_syntax_string((uchar*) ft_boolean_syntax,
                                     strlen(ft_boolean_syntax),
                                     system_charset_info))
  {
    sql_print_error("Invalid ft-boolean-syntax string: %s",
                    ft_boolean_syntax);
    return 1;
  }

  if (opt_disable_networking)
    mysqld_port= mysqld_extra_port= 0;

  if (opt_skip_show_db)
    opt_specialflag|= SPECIAL_SKIP_SHOW_DB;

  if (myisam_flush)
    flush_time= 0;

#ifdef HAVE_REPLICATION
  if (init_slave_skip_errors(opt_slave_skip_errors))
    return 1;
  if (init_slave_transaction_retry_errors(opt_slave_transaction_retry_errors))
    return 1;
#endif

  if (global_system_variables.max_join_size == HA_POS_ERROR)
    global_system_variables.option_bits|= OPTION_BIG_SELECTS;
  else
    global_system_variables.option_bits&= ~OPTION_BIG_SELECTS;

  if (opt_support_flashback)
  {
    /* Force binary logging */
    if (!opt_bin_logname)
      opt_bin_logname= (char*) "";                  // Use default name
    opt_bin_log= opt_bin_log_used= 1;

    /* Force format to row */
    binlog_format_used= 1;
    global_system_variables.binlog_format= BINLOG_FORMAT_ROW;
  }

  if (!opt_bootstrap && WSREP_PROVIDER_EXISTS && WSREP_ON &&
      global_system_variables.binlog_format != BINLOG_FORMAT_ROW)
  {

    WSREP_ERROR ("Only binlog_format = 'ROW' is currently supported. "
                 "Configured value: '%s'. Please adjust your configuration.",
                 binlog_format_names[global_system_variables.binlog_format]);
    return 1;
  }

  // Synchronize @@global.autocommit on --autocommit
  const ulonglong turn_bit_on= opt_autocommit ?
    OPTION_AUTOCOMMIT : OPTION_NOT_AUTOCOMMIT;
  global_system_variables.option_bits=
    (global_system_variables.option_bits &
     ~(OPTION_NOT_AUTOCOMMIT | OPTION_AUTOCOMMIT)) | turn_bit_on;

  global_system_variables.sql_mode=
    expand_sql_mode(global_system_variables.sql_mode);
#if !defined(HAVE_REALPATH) || defined(HAVE_BROKEN_REALPATH)
  my_use_symdir=0;
  my_disable_symlinks=1;
  have_symlink=SHOW_OPTION_NO;
#else
  if (!my_use_symdir)
  {
    my_disable_symlinks=1;
    have_symlink=SHOW_OPTION_DISABLED;
  }
#endif
  if (opt_debugging)
  {
    /* Allow break with SIGINT, no core or stack trace */
    test_flags|= TEST_SIGINT;
    test_flags&= ~TEST_CORE_ON_SIGNAL;
  }
  /* Set global MyISAM variables from delay_key_write_options */
  fix_delay_key_write(0, 0, OPT_GLOBAL);

#ifndef EMBEDDED_LIBRARY
  if (mysqld_chroot)
    set_root(mysqld_chroot);
#else
  SYSVAR_AUTOSIZE(thread_handling, SCHEDULER_NO_THREADS);
  max_allowed_packet= global_system_variables.max_allowed_packet;
  net_buffer_length= global_system_variables.net_buffer_length;
#endif
  if (fix_paths())
    return 1;

  /*
    Set some global variables from the global_system_variables
    In most cases the global variables will not be used
  */
  my_disable_locking= myisam_single_user= MY_TEST(opt_external_locking == 0);
  my_default_record_cache_size=global_system_variables.read_buff_size;

  /*
    Log mysys errors when we don't have a thd or thd->log_all_errors is set
    (recovery) to the log.  This is mainly useful for debugging strange system
    errors.
  */
  if (global_system_variables.log_warnings >= 10)
    my_global_flags= MY_WME | ME_NOTE;
  /* Log all errors not handled by thd->handle_error() to my_message_sql() */
  if (global_system_variables.log_warnings >= 11)
    my_global_flags|= ME_ERROR_LOG;
  if (my_assert_on_error)
    debug_assert_if_crashed_table= 1;

  global_system_variables.long_query_time= (ulonglong)
    (global_system_variables.long_query_time_double * 1e6 + 0.1);
  global_system_variables.max_statement_time= (ulonglong)
    (global_system_variables.max_statement_time_double * 1e6 + 0.1);

  if (opt_short_log_format)
    opt_specialflag|= SPECIAL_SHORT_LOG_FORMAT;

  if (init_global_datetime_format(MYSQL_TIMESTAMP_DATE,
                                  &global_date_format) ||
      init_global_datetime_format(MYSQL_TIMESTAMP_TIME,
                                  &global_time_format) ||
      init_global_datetime_format(MYSQL_TIMESTAMP_DATETIME,
                                  &global_datetime_format))
    return 1;

#ifdef EMBEDDED_LIBRARY
  one_thread_scheduler(thread_scheduler, &connection_count);
  /*
    It looks like extra_connection_count should be passed here but
    its been using connection_count for the last 10+ years and
    no-one was requested a change so lets not suprise anyone.
  */
  one_thread_scheduler(extra_thread_scheduler, &connection_count);
#else

  if (thread_handling <= SCHEDULER_ONE_THREAD_PER_CONNECTION)
    one_thread_per_connection_scheduler(thread_scheduler, &max_connections,
                                        &connection_count);
  else if (thread_handling == SCHEDULER_NO_THREADS)
    one_thread_scheduler(thread_scheduler, &connection_count);
  else
    pool_of_threads_scheduler(thread_scheduler,  &max_connections,
                                        &connection_count); 

  one_thread_per_connection_scheduler(extra_thread_scheduler,
                                      &extra_max_connections,
                                      &extra_connection_count);
#endif

  opt_readonly= read_only;

  /* Remember if max_user_connections was 0 at startup */
  max_user_connections_checking= global_system_variables.max_user_connections != 0;

#ifdef HAVE_REPLICATION
  {
    sys_var *max_relay_log_size_var, *max_binlog_size_var;
    /* If max_relay_log_size is 0, then set it to max_binlog_size */
    if (!global_system_variables.max_relay_log_size)
      SYSVAR_AUTOSIZE(global_system_variables.max_relay_log_size,
                      max_binlog_size);

    /*
      Fix so that DEFAULT and limit checking works with max_relay_log_size
      (Yes, this is a hack, but it's required as the definition of
      max_relay_log_size allows it to be set to 0).
    */
    max_relay_log_size_var= intern_find_sys_var(STRING_WITH_LEN("max_relay_log_size"));
    max_binlog_size_var= intern_find_sys_var(STRING_WITH_LEN("max_binlog_size"));
    if (max_binlog_size_var && max_relay_log_size_var)
    {
      max_relay_log_size_var->option.min_value=
        max_binlog_size_var->option.min_value; 
      max_relay_log_size_var->option.def_value=
        max_binlog_size_var->option.def_value;
    }
  }
#endif

  /* Ensure that some variables are not set higher than needed */
  if (thread_cache_size > max_connections)
    SYSVAR_AUTOSIZE(thread_cache_size, max_connections);

  return 0;
}


/*
  Create version name for running mysqld version
  We automaticly add suffixes -debug, -embedded and -log to the version
  name to make the version more descriptive.
  (MYSQL_SERVER_SUFFIX is set by the compilation environment)
*/

void set_server_version(char *buf, size_t size)
{
  bool is_log= opt_log || global_system_variables.sql_log_slow || opt_bin_log;
  bool is_debug= IF_DBUG(!strstr(MYSQL_SERVER_SUFFIX_STR, "-debug"), 0);
  const char *is_valgrind=
#ifdef HAVE_VALGRIND
    !strstr(MYSQL_SERVER_SUFFIX_STR, "-valgrind") ? "-valgrind" :
#endif
    "";
  strxnmov(buf, size - 1,
           MYSQL_SERVER_VERSION,
           MYSQL_SERVER_SUFFIX_STR,
           IF_EMBEDDED("-embedded", ""),
           is_valgrind,
           is_debug ? "-debug" : "",
           is_log ? "-log" : "",
           NullS);
}


static char *get_relative_path(const char *path)
{
  if (test_if_hard_path(path) &&
      is_prefix(path,DEFAULT_MYSQL_HOME) &&
      strcmp(DEFAULT_MYSQL_HOME,FN_ROOTDIR))
  {
    path+=(uint) strlen(DEFAULT_MYSQL_HOME);
    while (*path == FN_LIBCHAR || *path == FN_LIBCHAR2)
      path++;
  }
  return (char*) path;
}


/**
  Fix filename and replace extension where 'dir' is relative to
  mysql_real_data_home.
  @return
    1 if len(path) > FN_REFLEN
*/

bool
fn_format_relative_to_data_home(char * to, const char *name,
				const char *dir, const char *extension)
{
  char tmp_path[FN_REFLEN];
  if (!test_if_hard_path(dir))
  {
    strxnmov(tmp_path,sizeof(tmp_path)-1, mysql_real_data_home,
	     dir, NullS);
    dir=tmp_path;
  }
  return !fn_format(to, name, dir, extension,
		    MY_APPEND_EXT | MY_UNPACK_FILENAME | MY_SAFE_PATH);
}


/**
  Test a file path to determine if the path is compatible with the secure file
  path restriction.
 
  @param path null terminated character string

  @return
    @retval TRUE The path is secure
    @retval FALSE The path isn't secure
*/

bool is_secure_file_path(char *path)
{
  char buff1[FN_REFLEN], buff2[FN_REFLEN];
  size_t opt_secure_file_priv_len;
  /*
    All paths are secure if opt_secure_file_path is 0
  */
  if (!opt_secure_file_priv)
    return TRUE;

  opt_secure_file_priv_len= strlen(opt_secure_file_priv);

  if (strlen(path) >= FN_REFLEN)
    return FALSE;

  if (my_realpath(buff1, path, 0))
  {
    /*
      The supplied file path might have been a file and not a directory.
    */
    size_t length= dirname_length(path);        // Guaranteed to be < FN_REFLEN
    memcpy(buff2, path, length);
    buff2[length]= '\0';
    if (length == 0 || my_realpath(buff1, buff2, 0))
      return FALSE;
  }
  convert_dirname(buff2, buff1, NullS);
  if (!lower_case_file_system)
  {
    if (strncmp(opt_secure_file_priv, buff2, opt_secure_file_priv_len))
      return FALSE;
  }
  else
  {
    if (files_charset_info->strnncoll(buff2, strlen(buff2),
                                      opt_secure_file_priv,
                                      opt_secure_file_priv_len,
                                      TRUE))
      return FALSE;
  }
  return TRUE;
}


static int fix_paths(void)
{
  char buff[FN_REFLEN],*pos;
  DBUG_ENTER("fix_paths");

  convert_dirname(mysql_home,mysql_home,NullS);
  /* Resolve symlinks to allow 'mysql_home' to be a relative symlink */
  my_realpath(mysql_home,mysql_home,MYF(0));
  /* Ensure that mysql_home ends in FN_LIBCHAR */
  pos=strend(mysql_home);
  if (pos[-1] != FN_LIBCHAR)
  {
    pos[0]= FN_LIBCHAR;
    pos[1]= 0;
  }
  convert_dirname(lc_messages_dir, lc_messages_dir, NullS);
  convert_dirname(mysql_real_data_home,mysql_real_data_home,NullS);
  (void) my_load_path(mysql_home,mysql_home,""); // Resolve current dir
  (void) my_load_path(mysql_real_data_home,mysql_real_data_home,mysql_home);
  (void) my_load_path(pidfile_name, pidfile_name_ptr, mysql_real_data_home);

  convert_dirname(opt_plugin_dir, opt_plugin_dir_ptr ? opt_plugin_dir_ptr : 
                                  get_relative_path(PLUGINDIR), NullS);
  (void) my_load_path(opt_plugin_dir, opt_plugin_dir, mysql_home);
  opt_plugin_dir_ptr= opt_plugin_dir;
  pidfile_name_ptr= pidfile_name;

  my_realpath(mysql_unpacked_real_data_home, mysql_real_data_home, MYF(0));
  mysql_unpacked_real_data_home_len= 
  strlen(mysql_unpacked_real_data_home);
  if (mysql_unpacked_real_data_home[mysql_unpacked_real_data_home_len-1] == FN_LIBCHAR)
    --mysql_unpacked_real_data_home_len;

  char *sharedir=get_relative_path(SHAREDIR);
  if (test_if_hard_path(sharedir))
    strmake_buf(buff, sharedir);		/* purecov: tested */
  else
    strxnmov(buff,sizeof(buff)-1,mysql_home,sharedir,NullS);
  convert_dirname(buff,buff,NullS);
  (void) my_load_path(lc_messages_dir, lc_messages_dir, buff);

  /* If --character-sets-dir isn't given, use shared library dir */
  if (charsets_dir)
  {
    strmake_buf(mysql_charsets_dir, charsets_dir);
    charsets_dir= mysql_charsets_dir;
  }
  else
  {
    strxnmov(mysql_charsets_dir, sizeof(mysql_charsets_dir)-1, buff,
	     CHARSET_DIR, NullS);
    SYSVAR_AUTOSIZE(charsets_dir, mysql_charsets_dir);
  }
  (void) my_load_path(mysql_charsets_dir, mysql_charsets_dir, buff);
  convert_dirname(mysql_charsets_dir, mysql_charsets_dir, NullS);

  if (init_tmpdir(&mysql_tmpdir_list, opt_mysql_tmpdir))
    DBUG_RETURN(1);
  if (!opt_mysql_tmpdir)
    opt_mysql_tmpdir= mysql_tmpdir;
#ifdef HAVE_REPLICATION
  if (!slave_load_tmpdir)
    SYSVAR_AUTOSIZE(slave_load_tmpdir, mysql_tmpdir);
#endif /* HAVE_REPLICATION */
  /*
    Convert the secure-file-priv option to system format, allowing
    a quick strcmp to check if read or write is in an allowed dir
  */
  if (opt_secure_file_priv)
  {
    if (*opt_secure_file_priv == 0)
    {
      my_free(opt_secure_file_priv);
      opt_secure_file_priv= 0;
    }
    else
    {
      if (strlen(opt_secure_file_priv) >= FN_REFLEN)
        opt_secure_file_priv[FN_REFLEN-1]= '\0';
      if (my_realpath(buff, opt_secure_file_priv, 0))
      {
        sql_print_warning("Failed to normalize the argument for --secure-file-priv.");
        DBUG_RETURN(1);
      }
      char *secure_file_real_path= (char *)my_malloc(PSI_INSTRUMENT_ME, FN_REFLEN, MYF(MY_FAE));
      convert_dirname(secure_file_real_path, buff, NullS);
      my_free(opt_secure_file_priv);
      opt_secure_file_priv= secure_file_real_path;
    }
  }
  DBUG_RETURN(0);
}

/**
  Check if file system used for databases is case insensitive.

  @param dir_name			Directory to test

  @retval -1  Don't know (Test failed)
  @retval  0   File system is case sensitive
  @retval  1   File system is case insensitive
*/

static int test_if_case_insensitive(const char *dir_name)
{
  int result= 0;
  File file;
  char buff[FN_REFLEN], buff2[FN_REFLEN];
  MY_STAT stat_info;
  DBUG_ENTER("test_if_case_insensitive");

  fn_format(buff, opt_log_basename, dir_name, ".lower-test",
	    MY_UNPACK_FILENAME | MY_REPLACE_EXT | MY_REPLACE_DIR);
  fn_format(buff2, opt_log_basename, dir_name, ".LOWER-TEST",
	    MY_UNPACK_FILENAME | MY_REPLACE_EXT | MY_REPLACE_DIR);
  mysql_file_delete(key_file_casetest, buff2, MYF(0));
  if ((file= mysql_file_create(key_file_casetest,
                               buff, 0666, O_RDWR, MYF(0))) < 0)
  {
    if (!opt_abort)
      sql_print_warning("Can't create test file %s", buff);
    DBUG_RETURN(-1);
  }
  mysql_file_close(file, MYF(0));
  if (mysql_file_stat(key_file_casetest, buff2, &stat_info, MYF(0)))
    result= 1;					// Can access file
  mysql_file_delete(key_file_casetest, buff, MYF(MY_WME));
  DBUG_PRINT("exit", ("result: %d", result));
  DBUG_RETURN(result);
}


#ifndef EMBEDDED_LIBRARY

/**
  Create file to store pid number.
*/
static void create_pid_file()
{
  File file;
  if ((file= mysql_file_create(key_file_pid, pidfile_name, 0664,
                               O_WRONLY | O_TRUNC, MYF(MY_WME))) >= 0)
  {
    char buff[MAX_BIGINT_WIDTH + 1], *end;
    end= int10_to_str((long) getpid(), buff, 10);
    *end++= '\n';
    if (!mysql_file_write(file, (uchar*) buff, (uint) (end-buff),
                          MYF(MY_WME | MY_NABP)))
    {
      mysql_file_close(file, MYF(0));
      pid_file_created= true;
      return;
    }
    mysql_file_close(file, MYF(0));
  }
  sql_perror("Can't start server: can't create PID file");
  exit(1);
}
#endif /* EMBEDDED_LIBRARY */


/**
  Remove the process' pid file.
  
  @param  flags  file operation flags
*/

static void delete_pid_file(myf flags)
{
#ifndef EMBEDDED_LIBRARY
  if (pid_file_created)
  {
    mysql_file_delete(key_file_pid, pidfile_name, flags);
    pid_file_created= false;
  }
#endif /* EMBEDDED_LIBRARY */
  return;
}


/** Clear most status variables. */
void refresh_status(THD *thd)
{
  mysql_mutex_lock(&LOCK_status);

#ifdef WITH_PERFSCHEMA_STORAGE_ENGINE
  /* Reset aggregated status counters. */
  reset_pfs_status_stats();
#endif

  /* Add thread's status variabes to global status */
  add_to_status(&global_status_var, &thd->status_var);

  /* Reset thread's status variables */
  thd->set_status_var_init();
  thd->status_var.global_memory_used= 0;
  bzero((uchar*) &thd->org_status_var, sizeof(thd->org_status_var)); 
  thd->start_bytes_received= 0;

  /* Reset some global variables */
  reset_status_vars();
#ifdef WITH_WSREP
  if (WSREP_ON)
  {
    Wsrep_server_state::instance().provider().reset_status();
  }
#endif /* WITH_WSREP */

  /* Reset the counters of all key caches (default and named). */
  process_key_caches(reset_key_cache_counters, 0);
  flush_status_time= time((time_t*) 0);
  mysql_mutex_unlock(&LOCK_status);

  /*
    Set max_used_connections to the number of currently open
    connections.  This is not perfect, but status data is not exact anyway.
  */
  max_used_connections= connection_count + extra_connection_count;
}

#ifdef HAVE_PSI_INTERFACE
static PSI_file_info all_server_files[]=
{
#ifdef HAVE_MMAP
  { &key_file_map, "map", 0},
#endif /* HAVE_MMAP */
  { &key_file_binlog, "binlog", 0},
  { &key_file_binlog_cache, "binlog_cache", 0},
  { &key_file_binlog_index, "binlog_index", 0},
  { &key_file_binlog_index_cache, "binlog_index_cache", 0},
  { &key_file_relaylog, "relaylog", 0},
  { &key_file_relaylog_cache, "relaylog_cache", 0},
  { &key_file_relaylog_index, "relaylog_index", 0},
  { &key_file_relaylog_index_cache, "relaylog_index_cache", 0},
  { &key_file_io_cache, "io_cache", 0},
  { &key_file_casetest, "casetest", 0},
  { &key_file_dbopt, "dbopt", 0},
  { &key_file_des_key_file, "des_key_file", 0},
  { &key_file_ERRMSG, "ERRMSG", 0},
  { &key_select_to_file, "select_to_file", 0},
  { &key_file_fileparser, "file_parser", 0},
  { &key_file_frm, "FRM", 0},
  { &key_file_global_ddl_log, "global_ddl_log", 0},
  { &key_file_load, "load", 0},
  { &key_file_loadfile, "LOAD_FILE", 0},
  { &key_file_log_ddl, "log_ddl", 0},
  { &key_file_log_event_data, "log_event_data", 0},
  { &key_file_log_event_info, "log_event_info", 0},
  { &key_file_master_info, "master_info", 0},
  { &key_file_misc, "misc", 0},
  { &key_file_partition_ddl_log, "partition_ddl_log", 0},
  { &key_file_pid, "pid", 0},
  { &key_file_query_log, "query_log", 0},
  { &key_file_relay_log_info, "relay_log_info", 0},
  { &key_file_send_file, "send_file", 0},
  { &key_file_slow_log, "slow_log", 0},
  { &key_file_tclog, "tclog", 0},
  { &key_file_trg, "trigger_name", 0},
  { &key_file_trn, "trigger", 0},
  { &key_file_init, "init", 0},
  { &key_file_binlog_state, "binlog_state", 0}
};
#endif /* HAVE_PSI_INTERFACE */

PSI_stage_info stage_after_apply_event= { 0, "After apply log event", 0};
PSI_stage_info stage_after_create= { 0, "After create", 0};
PSI_stage_info stage_after_opening_tables= { 0, "After opening tables", 0};
PSI_stage_info stage_after_table_lock= { 0, "After table lock", 0};
PSI_stage_info stage_allocating_local_table= { 0, "Allocating local table", 0};
PSI_stage_info stage_alter_inplace_prepare= { 0, "preparing for alter table", 0};
PSI_stage_info stage_alter_inplace= { 0, "altering table", 0};
PSI_stage_info stage_alter_inplace_commit= { 0, "Committing alter table to storage engine", 0};
PSI_stage_info stage_apply_event= { 0, "Apply log event", 0};
PSI_stage_info stage_changing_master= { 0, "Changing master", 0};
PSI_stage_info stage_checking_master_version= { 0, "Checking master version", 0};
PSI_stage_info stage_checking_permissions= { 0, "checking permissions", 0};
PSI_stage_info stage_checking_privileges_on_cached_query= { 0, "checking privileges on cached query", 0};
PSI_stage_info stage_checking_query_cache_for_query= { 0, "Checking query cache for query", 0};
PSI_stage_info stage_cleaning_up= { 0, "Reset for next command", 0};
PSI_stage_info stage_closing_tables= { 0, "closing tables", 0};
PSI_stage_info stage_connecting_to_master= { 0, "Connecting to master", 0};
PSI_stage_info stage_converting_heap_to_myisam= { 0, "Converting HEAP to " TMP_ENGINE_NAME, 0};
PSI_stage_info stage_copying_to_group_table= { 0, "Copying to group table", 0};
PSI_stage_info stage_copying_to_tmp_table= { 0, "Copying to tmp table", 0};
PSI_stage_info stage_copy_to_tmp_table= { 0, "copy to tmp table", PSI_FLAG_STAGE_PROGRESS};
PSI_stage_info stage_creating_delayed_handler= { 0, "Creating delayed handler", 0};
PSI_stage_info stage_creating_sort_index= { 0, "Creating sort index", 0};
PSI_stage_info stage_creating_table= { 0, "creating table", 0};
PSI_stage_info stage_creating_tmp_table= { 0, "Creating tmp table", 0};
PSI_stage_info stage_deleting_from_main_table= { 0, "Deleting from main table", 0};
PSI_stage_info stage_deleting_from_reference_tables= { 0, "Deleting from reference tables", 0};
PSI_stage_info stage_discard_or_import_tablespace= { 0, "Discard_or_import_tablespace", 0};
PSI_stage_info stage_enabling_keys= { 0, "Enabling keys", 0};
PSI_stage_info stage_end= { 0, "End of update loop", 0};
PSI_stage_info stage_executing= { 0, "Executing", 0};
PSI_stage_info stage_execution_of_init_command= { 0, "Execution of init_command", 0};
PSI_stage_info stage_explaining= { 0, "Explaining", 0};
PSI_stage_info stage_finding_key_cache= { 0, "Finding key cache", 0};
PSI_stage_info stage_finished_reading_one_binlog_switching_to_next_binlog= { 0, "Finished reading one binlog; switching to next binlog", 0};
PSI_stage_info stage_flushing_relay_log_and_master_info_repository= { 0, "Flushing relay log and master info repository.", 0};
PSI_stage_info stage_flushing_relay_log_info_file= { 0, "Flushing relay-log info file.", 0};
PSI_stage_info stage_freeing_items= { 0, "Freeing items", 0};
PSI_stage_info stage_fulltext_initialization= { 0, "Fulltext initialization", 0};
PSI_stage_info stage_got_handler_lock= { 0, "Got handler lock", 0};
PSI_stage_info stage_got_old_table= { 0, "Got old table", 0};
PSI_stage_info stage_init= { 0, "init", 0};
PSI_stage_info stage_init_update= { 0, "init for update", 0};
PSI_stage_info stage_insert= { 0, "Insert", 0};
PSI_stage_info stage_invalidating_query_cache_entries_table= { 0, "Invalidating query cache entries (table)", 0};
PSI_stage_info stage_invalidating_query_cache_entries_table_list= { 0, "Invalidating query cache entries (table list)", 0};
PSI_stage_info stage_killing_slave= { 0, "Killing slave", 0};
PSI_stage_info stage_logging_slow_query= { 0, "Logging slow query", 0};
PSI_stage_info stage_making_temp_file_append_before_load_data= { 0, "Making temporary file (append) before replaying LOAD DATA INFILE", 0};
PSI_stage_info stage_making_temp_file_create_before_load_data= { 0, "Making temporary file (create) before replaying LOAD DATA INFILE", 0};
PSI_stage_info stage_manage_keys= { 0, "Manage keys", 0};
PSI_stage_info stage_master_has_sent_all_binlog_to_slave= { 0, "Master has sent all binlog to slave; waiting for more updates", 0};
PSI_stage_info stage_opening_tables= { 0, "Opening tables", 0};
PSI_stage_info stage_optimizing= { 0, "Optimizing", 0};
PSI_stage_info stage_preparing= { 0, "Preparing", 0};
PSI_stage_info stage_purging_old_relay_logs= { 0, "Purging old relay logs", 0};
PSI_stage_info stage_query_end= { 0, "Query end", 0};
PSI_stage_info stage_starting_cleanup= { 0, "Starting cleanup", 0};
PSI_stage_info stage_rollback= { 0, "Rollback", 0};
PSI_stage_info stage_rollback_implicit= { 0, "Rollback_implicit", 0};
PSI_stage_info stage_commit= { 0, "Commit", 0};
PSI_stage_info stage_commit_implicit= { 0, "Commit implicit", 0};
PSI_stage_info stage_queueing_master_event_to_the_relay_log= { 0, "Queueing master event to the relay log", 0};
PSI_stage_info stage_reading_event_from_the_relay_log= { 0, "Reading event from the relay log", 0};
PSI_stage_info stage_recreating_table= { 0, "Recreating table", 0};
PSI_stage_info stage_registering_slave_on_master= { 0, "Registering slave on master", 0};
PSI_stage_info stage_removing_duplicates= { 0, "Removing duplicates", 0};
PSI_stage_info stage_removing_tmp_table= { 0, "Removing tmp table", 0};
PSI_stage_info stage_rename= { 0, "Rename", 0};
PSI_stage_info stage_rename_result_table= { 0, "Rename result table", 0};
PSI_stage_info stage_requesting_binlog_dump= { 0, "Requesting binlog dump", 0};
PSI_stage_info stage_reschedule= { 0, "Reschedule", 0};
PSI_stage_info stage_searching_rows_for_update= { 0, "Searching rows for update", 0};
PSI_stage_info stage_sending_binlog_event_to_slave= { 0, "Sending binlog event to slave", 0};
PSI_stage_info stage_sending_cached_result_to_client= { 0, "Sending cached result to client", 0};
PSI_stage_info stage_sending_data= { 0, "Sending data", 0};
PSI_stage_info stage_setup= { 0, "setup", 0};
PSI_stage_info stage_show_explain= { 0, "Show explain", 0};
PSI_stage_info stage_slave_has_read_all_relay_log= { 0, "Slave has read all relay log; waiting for more updates", 0};
PSI_stage_info stage_sorting= { 0, "Sorting", 0};
PSI_stage_info stage_sorting_for_group= { 0, "Sorting for group", 0};
PSI_stage_info stage_sorting_for_order= { 0, "Sorting for order", 0};
PSI_stage_info stage_sorting_result= { 0, "Sorting result", 0};
PSI_stage_info stage_statistics= { 0, "Statistics", 0};
PSI_stage_info stage_sql_thd_waiting_until_delay= { 0, "Waiting until MASTER_DELAY seconds after master executed event", 0 };
PSI_stage_info stage_storing_result_in_query_cache= { 0, "Storing result in query cache", 0};
PSI_stage_info stage_storing_row_into_queue= { 0, "Storing row into queue", 0};
PSI_stage_info stage_system_lock= { 0, "System lock", 0};
PSI_stage_info stage_unlocking_tables= { 0, "Unlocking tables", 0};
PSI_stage_info stage_table_lock= { 0, "table lock", 0};
PSI_stage_info stage_filling_schema_table= { 0, "Filling schema table", 0};
PSI_stage_info stage_update= { 0, "Update", 0};
PSI_stage_info stage_updating= { 0, "Updating", 0};
PSI_stage_info stage_updating_main_table= { 0, "Updating main table", 0};
PSI_stage_info stage_updating_reference_tables= { 0, "Updating reference tables", 0};
PSI_stage_info stage_upgrading_lock= { 0, "Upgrading lock", 0};
PSI_stage_info stage_user_lock= { 0, "User lock", 0};
PSI_stage_info stage_user_sleep= { 0, "User sleep", 0};
PSI_stage_info stage_verifying_table= { 0, "Verifying table", 0};
PSI_stage_info stage_waiting_for_delay_list= { 0, "Waiting for delay_list", 0};
PSI_stage_info stage_waiting_for_gtid_to_be_written_to_binary_log= { 0, "Waiting for GTID to be written to binary log", 0};
PSI_stage_info stage_waiting_for_handler_insert= { 0, "Waiting for handler insert", 0};
PSI_stage_info stage_waiting_for_handler_lock= { 0, "Waiting for handler lock", 0};
PSI_stage_info stage_waiting_for_handler_open= { 0, "Waiting for handler open", 0};
PSI_stage_info stage_waiting_for_insert= { 0, "Waiting for INSERT", 0};
PSI_stage_info stage_waiting_for_master_to_send_event= { 0, "Waiting for master to send event", 0};
PSI_stage_info stage_waiting_for_master_update= { 0, "Waiting for master update", 0};
PSI_stage_info stage_waiting_for_relay_log_space= { 0, "Waiting for the slave SQL thread to free enough relay log space", 0};
PSI_stage_info stage_waiting_for_semi_sync_ack_from_slave=
{ 0, "Waiting for semi-sync ACK from slave", 0};
PSI_stage_info stage_waiting_for_semi_sync_slave={ 0, "Waiting for semi-sync slave connection", 0};
PSI_stage_info stage_reading_semi_sync_ack={ 0, "Reading semi-sync ACK from slave", 0};
PSI_stage_info stage_waiting_for_slave_mutex_on_exit= { 0, "Waiting for slave mutex on exit", 0};
PSI_stage_info stage_waiting_for_slave_thread_to_start= { 0, "Waiting for slave thread to start", 0};
PSI_stage_info stage_waiting_for_table_flush= { 0, "Waiting for table flush", 0};
PSI_stage_info stage_waiting_for_query_cache_lock= { 0, "Waiting for query cache lock", 0};
PSI_stage_info stage_waiting_for_the_next_event_in_relay_log= { 0, "Waiting for the next event in relay log", 0};
PSI_stage_info stage_waiting_for_the_slave_thread_to_advance_position= { 0, "Waiting for the slave SQL thread to advance position", 0};
PSI_stage_info stage_waiting_to_finalize_termination= { 0, "Waiting to finalize termination", 0};
PSI_stage_info stage_binlog_waiting_background_tasks= { 0, "Waiting for background binlog tasks", 0};
PSI_stage_info stage_binlog_write= { 0, "Writing to binlog", 0};
PSI_stage_info stage_binlog_processing_checkpoint_notify= { 0, "Processing binlog checkpoint notification", 0};
PSI_stage_info stage_binlog_stopping_background_thread= { 0, "Stopping binlog background thread", 0};
PSI_stage_info stage_waiting_for_work_from_sql_thread= { 0, "Waiting for work from SQL thread", 0};
PSI_stage_info stage_waiting_for_prior_transaction_to_commit= { 0, "Waiting for prior transaction to commit", 0};
PSI_stage_info stage_waiting_for_prior_transaction_to_start_commit= { 0, "Waiting for prior transaction to start commit", 0};
PSI_stage_info stage_waiting_for_room_in_worker_thread= { 0, "Waiting for room in worker thread event queue", 0};
PSI_stage_info stage_waiting_for_workers_idle= { 0, "Waiting for worker threads to be idle", 0};
PSI_stage_info stage_waiting_for_ftwrl= { 0, "Waiting due to global read lock", 0};
PSI_stage_info stage_waiting_for_ftwrl_threads_to_pause= { 0, "Waiting for worker threads to pause for global read lock", 0};
PSI_stage_info stage_waiting_for_rpl_thread_pool= { 0, "Waiting while replication worker thread pool is busy", 0};
PSI_stage_info stage_master_gtid_wait_primary= { 0, "Waiting in MASTER_GTID_WAIT() (primary waiter)", 0};
PSI_stage_info stage_master_gtid_wait= { 0, "Waiting in MASTER_GTID_WAIT()", 0};
PSI_stage_info stage_gtid_wait_other_connection= { 0, "Waiting for other master connection to process the same GTID", 0};
PSI_stage_info stage_slave_background_process_request= { 0, "Processing requests", 0};
PSI_stage_info stage_slave_background_wait_request= { 0, "Waiting for requests", 0};
PSI_stage_info stage_waiting_for_deadlock_kill= { 0, "Waiting for parallel replication deadlock handling to complete", 0};
PSI_stage_info stage_starting= { 0, "starting", 0};
PSI_stage_info stage_waiting_for_flush= { 0, "Waiting for non trans tables to be flushed", 0};
PSI_stage_info stage_waiting_for_ddl= { 0, "Waiting for DDLs", 0};

#ifdef WITH_WSREP
// Aditional Galera thread states
PSI_stage_info stage_waiting_isolation= { 0, "Waiting to execute in isolation", 0};
PSI_stage_info stage_waiting_certification= {0, "Waiting for certification", 0};
PSI_stage_info stage_waiting_ddl= {0, "Waiting for TOI DDL", 0};
PSI_stage_info stage_waiting_flow= {0, "Waiting for flow control", 0};
#endif /* WITH_WSREP */

PSI_memory_key key_memory_DATE_TIME_FORMAT;
PSI_memory_key key_memory_DDL_LOG_MEMORY_ENTRY;
PSI_memory_key key_memory_Event_queue_element_for_exec_names;
PSI_memory_key key_memory_Event_scheduler_scheduler_param;
PSI_memory_key key_memory_Filesort_info_merge;
PSI_memory_key key_memory_Filesort_info_record_pointers;
PSI_memory_key key_memory_Gis_read_stream_err_msg;
PSI_memory_key key_memory_JOIN_CACHE;
PSI_memory_key key_memory_MPVIO_EXT_auth_info;
PSI_memory_key key_memory_MYSQL_BIN_LOG_basename;
PSI_memory_key key_memory_MYSQL_BIN_LOG_index;
PSI_memory_key key_memory_MYSQL_LOCK;
PSI_memory_key key_memory_MYSQL_LOG_name;
PSI_memory_key key_memory_MYSQL_RELAY_LOG_basename;
PSI_memory_key key_memory_MYSQL_RELAY_LOG_index;
PSI_memory_key key_memory_NAMED_ILINK_name;
PSI_memory_key key_memory_PROFILE;
PSI_memory_key key_memory_QUICK_RANGE_SELECT_mrr_buf_desc;
PSI_memory_key key_memory_Query_cache;
PSI_memory_key key_memory_Relay_log_info_group_relay_log_name;
PSI_memory_key key_memory_Row_data_memory_memory;
PSI_memory_key key_memory_Rpl_info_file_buffer;
PSI_memory_key key_memory_SLAVE_INFO;
PSI_memory_key key_memory_ST_SCHEMA_TABLE;
PSI_memory_key key_memory_Sort_param_tmp_buffer;
PSI_memory_key key_memory_Sys_var_charptr_value;
PSI_memory_key key_memory_TABLE;
PSI_memory_key key_memory_TABLE_RULE_ENT;
PSI_memory_key key_memory_TC_LOG_MMAP_pages;
PSI_memory_key key_memory_THD_db;
PSI_memory_key key_memory_THD_handler_tables_hash;
PSI_memory_key key_memory_THD_variables;
PSI_memory_key key_memory_Table_trigger_dispatcher;
PSI_memory_key key_memory_Unique_merge_buffer;
PSI_memory_key key_memory_Unique_sort_buffer;
PSI_memory_key key_memory_User_level_lock;
PSI_memory_key key_memory_XID;
PSI_memory_key key_memory_acl_cache;
PSI_memory_key key_memory_acl_mem;
PSI_memory_key key_memory_acl_memex;
PSI_memory_key key_memory_binlog_cache_mngr;
PSI_memory_key key_memory_binlog_pos;
PSI_memory_key key_memory_binlog_recover_exec;
PSI_memory_key key_memory_binlog_statement_buffer;
PSI_memory_key key_memory_binlog_ver_1_event;
PSI_memory_key key_memory_bison_stack;
PSI_memory_key key_memory_blob_mem_storage;
PSI_memory_key key_memory_dboptions_hash;
PSI_memory_key key_memory_dbnames_cache;
PSI_memory_key key_memory_errmsgs;
PSI_memory_key key_memory_frm_string;
PSI_memory_key key_memory_gdl;
PSI_memory_key key_memory_global_system_variables;
PSI_memory_key key_memory_handler_errmsgs;
PSI_memory_key key_memory_handlerton;
PSI_memory_key key_memory_hash_index_key_buffer;
PSI_memory_key key_memory_host_cache_hostname;
PSI_memory_key key_memory_ignored_db;
PSI_memory_key key_memory_locked_table_list;
PSI_memory_key key_memory_locked_thread_list;
PSI_memory_key key_memory_my_str_malloc;
PSI_memory_key key_memory_native_functions;
PSI_memory_key key_memory_prepared_statement_main_mem_root;
PSI_memory_key key_memory_prepared_statement_map;
PSI_memory_key key_memory_queue_item;
PSI_memory_key key_memory_quick_range_select_root;
PSI_memory_key key_memory_rpl_filter;
PSI_memory_key key_memory_sp_cache;
PSI_memory_key key_memory_sp_head_call_root;
PSI_memory_key key_memory_sp_head_execute_root;
PSI_memory_key key_memory_sp_head_main_root;
PSI_memory_key key_memory_table_mapping_root;
PSI_memory_key key_memory_table_share;
PSI_memory_key key_memory_table_triggers_list;
PSI_memory_key key_memory_thd_main_mem_root;
PSI_memory_key key_memory_thd_transactions;
PSI_memory_key key_memory_user_conn;
PSI_memory_key key_memory_user_var_entry;
PSI_memory_key key_memory_user_var_entry_value;

PSI_memory_key key_memory_String_value;

#ifdef HAVE_PSI_INTERFACE

PSI_stage_info *all_server_stages[]=
{
  & stage_after_apply_event,
  & stage_after_create,
  & stage_after_opening_tables,
  & stage_after_table_lock,
  & stage_allocating_local_table,
  & stage_alter_inplace,
  & stage_alter_inplace_commit,
  & stage_alter_inplace_prepare,
  & stage_apply_event,
  & stage_binlog_write,
  & stage_binlog_processing_checkpoint_notify,
  & stage_binlog_stopping_background_thread,
  & stage_binlog_waiting_background_tasks,
  & stage_changing_master,
  & stage_checking_master_version,
  & stage_checking_permissions,
  & stage_checking_privileges_on_cached_query,
  & stage_checking_query_cache_for_query,
  & stage_cleaning_up,
  & stage_closing_tables,
  & stage_commit,
  & stage_commit_implicit,
  & stage_connecting_to_master,
  & stage_converting_heap_to_myisam,
  & stage_copy_to_tmp_table,
  & stage_copying_to_group_table,
  & stage_copying_to_tmp_table,
  & stage_creating_delayed_handler,
  & stage_creating_sort_index,
  & stage_creating_table,
  & stage_creating_tmp_table,
  & stage_deleting_from_main_table,
  & stage_deleting_from_reference_tables,
  & stage_discard_or_import_tablespace,
  & stage_enabling_keys,
  & stage_end,
  & stage_executing,
  & stage_execution_of_init_command,
  & stage_explaining,
  & stage_finding_key_cache,
  & stage_finished_reading_one_binlog_switching_to_next_binlog,
  & stage_flushing_relay_log_and_master_info_repository,
  & stage_flushing_relay_log_info_file,
  & stage_freeing_items,
  & stage_fulltext_initialization,
  & stage_got_handler_lock,
  & stage_got_old_table,
  & stage_init,
  & stage_init_update,
  & stage_insert,
  & stage_invalidating_query_cache_entries_table,
  & stage_invalidating_query_cache_entries_table_list,
  & stage_killing_slave,
  & stage_logging_slow_query,
  & stage_making_temp_file_append_before_load_data,
  & stage_making_temp_file_create_before_load_data,
  & stage_manage_keys,
  & stage_master_has_sent_all_binlog_to_slave,
  & stage_opening_tables,
  & stage_optimizing,
  & stage_preparing,
  & stage_purging_old_relay_logs,
  & stage_starting_cleanup,
  & stage_query_end,
  & stage_queueing_master_event_to_the_relay_log,
  & stage_reading_event_from_the_relay_log,
  & stage_recreating_table,
  & stage_registering_slave_on_master,
  & stage_removing_duplicates,
  & stage_removing_tmp_table,
  & stage_rename,
  & stage_rename_result_table,
  & stage_requesting_binlog_dump,
  & stage_reschedule,
  & stage_rollback,
  & stage_rollback_implicit,
  & stage_searching_rows_for_update,
  & stage_sending_binlog_event_to_slave,
  & stage_sending_cached_result_to_client,
  & stage_sending_data,
  & stage_setup,
  & stage_show_explain,
  & stage_slave_has_read_all_relay_log,
  & stage_sorting,
  & stage_sorting_for_group,
  & stage_sorting_for_order,
  & stage_sorting_result,
  & stage_sql_thd_waiting_until_delay,
  & stage_statistics,
  & stage_storing_result_in_query_cache,
  & stage_storing_row_into_queue,
  & stage_system_lock,
  & stage_unlocking_tables,
  & stage_table_lock,
  & stage_filling_schema_table,
  & stage_update,
  & stage_updating,
  & stage_updating_main_table,
  & stage_updating_reference_tables,
  & stage_upgrading_lock,
  & stage_user_lock,
  & stage_user_sleep,
  & stage_verifying_table,
  & stage_waiting_for_delay_list,
  & stage_waiting_for_gtid_to_be_written_to_binary_log,
  & stage_waiting_for_handler_insert,
  & stage_waiting_for_handler_lock,
  & stage_waiting_for_handler_open,
  & stage_waiting_for_insert,
  & stage_waiting_for_master_to_send_event,
  & stage_waiting_for_master_update,
  & stage_waiting_for_prior_transaction_to_commit,
  & stage_waiting_for_prior_transaction_to_start_commit,
  & stage_waiting_for_query_cache_lock,
  & stage_waiting_for_relay_log_space,
  & stage_waiting_for_room_in_worker_thread,
  & stage_waiting_for_slave_mutex_on_exit,
  & stage_waiting_for_slave_thread_to_start,
  & stage_waiting_for_table_flush,
  & stage_waiting_for_the_next_event_in_relay_log,
  & stage_waiting_for_the_slave_thread_to_advance_position,
  & stage_waiting_for_work_from_sql_thread,
  & stage_waiting_to_finalize_termination,
  & stage_master_gtid_wait_primary,
  & stage_master_gtid_wait,
  & stage_gtid_wait_other_connection,
  & stage_slave_background_process_request,
  & stage_slave_background_wait_request,
  & stage_waiting_for_semi_sync_ack_from_slave,
  & stage_waiting_for_semi_sync_slave,
  & stage_reading_semi_sync_ack,
  & stage_waiting_for_deadlock_kill,
  & stage_starting
#ifdef WITH_WSREP
  ,
  & stage_waiting_isolation,
  & stage_waiting_certification,
  & stage_waiting_ddl,
  & stage_waiting_flow
#endif /* WITH_WSREP */
};

PSI_socket_key key_socket_tcpip, key_socket_unix, key_socket_client_connection;

static PSI_socket_info all_server_sockets[]=
{
  { &key_socket_tcpip, "server_tcpip_socket", PSI_FLAG_GLOBAL},
  { &key_socket_unix, "server_unix_socket", PSI_FLAG_GLOBAL},
  { &key_socket_client_connection, "client_connection", 0}
};

static PSI_memory_info all_server_memory[]=
{
  { &key_memory_locked_table_list, "Locked_tables_list::m_locked_tables_root", 0},
  { &key_memory_locked_thread_list, "display_table_locks", PSI_FLAG_THREAD},
  { &key_memory_thd_transactions, "THD::transactions::mem_root", PSI_FLAG_THREAD},
//  { &key_memory_delegate, "Delegate::memroot", 0},
  { &key_memory_acl_mem, "sql_acl_mem", PSI_FLAG_GLOBAL},
  { &key_memory_acl_memex, "sql_acl_memex", PSI_FLAG_GLOBAL},
  { &key_memory_acl_cache, "acl_cache", PSI_FLAG_GLOBAL},
  { &key_memory_thd_main_mem_root, "thd::main_mem_root", PSI_FLAG_THREAD},
//  { &key_memory_help, "help", 0},
//  { &key_memory_new_frm_mem, "new_frm_mem", 0},
  { &key_memory_table_share, "TABLE_SHARE::mem_root", PSI_FLAG_GLOBAL}, /* table definition cache */
  { &key_memory_gdl, "gdl", 0},
  { &key_memory_table_triggers_list, "Table_triggers_list", 0},
//  { &key_memory_servers, "servers", 0},
  { &key_memory_prepared_statement_map, "Prepared_statement_map", PSI_FLAG_THREAD},
  { &key_memory_prepared_statement_main_mem_root, "Prepared_statement::main_mem_root", PSI_FLAG_THREAD},
//  { &key_memory_protocol_rset_root, "Protocol_local::m_rset_root", PSI_FLAG_THREAD},
//  { &key_memory_warning_info_warn_root, "Warning_info::m_warn_root", PSI_FLAG_THREAD},
  { &key_memory_sp_cache, "THD::sp_cache", 0},
  { &key_memory_sp_head_main_root, "sp_head::main_mem_root", 0},
  { &key_memory_sp_head_execute_root, "sp_head::execute_mem_root", PSI_FLAG_THREAD},
  { &key_memory_sp_head_call_root, "sp_head::call_mem_root", PSI_FLAG_THREAD},
  { &key_memory_table_mapping_root, "table_mapping::m_mem_root", 0},
  { &key_memory_quick_range_select_root, "QUICK_RANGE_SELECT::alloc", PSI_FLAG_THREAD},
//  { &key_memory_quick_index_merge_root, "QUICK_INDEX_MERGE_SELECT::alloc", PSI_FLAG_THREAD},
//  { &key_memory_quick_ror_intersect_select_root, "QUICK_ROR_INTERSECT_SELECT::alloc", PSI_FLAG_THREAD},
//  { &key_memory_quick_ror_union_select_root, "QUICK_ROR_UNION_SELECT::alloc", PSI_FLAG_THREAD},
//  { &key_memory_quick_group_min_max_select_root, "QUICK_GROUP_MIN_MAX_SELECT::alloc", PSI_FLAG_THREAD},
//  { &key_memory_test_quick_select_exec, "test_quick_select", PSI_FLAG_THREAD},
//  { &key_memory_prune_partitions_exec, "prune_partitions::exec", 0},
  { &key_memory_binlog_recover_exec, "MYSQL_BIN_LOG::recover", 0},
  { &key_memory_blob_mem_storage, "Blob_mem_storage::storage", 0},
  { &key_memory_NAMED_ILINK_name, "NAMED_ILINK::name", 0},
  { &key_memory_String_value, "String::value", 0},
  { &key_memory_Sys_var_charptr_value, "Sys_var_charptr::value", 0},
  { &key_memory_queue_item, "Queue::queue_item", 0},
  { &key_memory_THD_db, "THD::db", 0},
  { &key_memory_user_var_entry, "user_var_entry", 0},
//  { &key_memory_Slave_job_group_group_relay_log_name, "Slave_job_group::group_relay_log_name", 0},
  { &key_memory_Relay_log_info_group_relay_log_name, "Relay_log_info::group_relay_log_name", 0},
  { &key_memory_binlog_cache_mngr, "binlog_cache_mngr", 0},
  { &key_memory_Row_data_memory_memory, "Row_data_memory::memory", 0},
//  { &key_memory_Gtid_set_to_string, "Gtid_set::to_string", 0},
//  { &key_memory_Gtid_state_to_string, "Gtid_state::to_string", 0},
//  { &key_memory_Owned_gtids_to_string, "Owned_gtids::to_string", 0},
//  { &key_memory_log_event, "Log_event", 0},
//  { &key_memory_Incident_log_event_message, "Incident_log_event::message", 0},
//  { &key_memory_Rows_query_log_event_rows_query, "Rows_query_log_event::rows_query", 0},
  { &key_memory_Sort_param_tmp_buffer, "Sort_param::tmp_buffer", 0},
  { &key_memory_Filesort_info_merge, "Filesort_info::merge", 0},
  { &key_memory_Filesort_info_record_pointers, "Filesort_info::record_pointers", 0},
//  { &key_memory_Filesort_buffer_sort_keys, "Filesort_buffer::sort_keys", 0},
  { &key_memory_handler_errmsgs, "handler::errmsgs", 0},
  { &key_memory_handlerton, "handlerton", 0},
  { &key_memory_XID, "XID", 0},
  { &key_memory_host_cache_hostname, "host_cache::hostname", 0},
  { &key_memory_user_var_entry_value, "user_var_entry::value", 0},
  { &key_memory_User_level_lock, "User_level_lock", 0},
  { &key_memory_MYSQL_LOG_name, "MYSQL_LOG::name", 0},
  { &key_memory_TC_LOG_MMAP_pages, "TC_LOG_MMAP::pages", 0},
//  { &key_memory_my_bitmap_map, "my_bitmap_map", 0},
  { &key_memory_QUICK_RANGE_SELECT_mrr_buf_desc, "QUICK_RANGE_SELECT::mrr_buf_desc", 0},
  { &key_memory_Event_queue_element_for_exec_names, "Event_queue_element_for_exec::names", 0},
  { &key_memory_my_str_malloc, "my_str_malloc", 0},
  { &key_memory_MYSQL_BIN_LOG_basename, "MYSQL_BIN_LOG::basename", 0},
  { &key_memory_MYSQL_BIN_LOG_index, "MYSQL_BIN_LOG::index", 0},
  { &key_memory_MYSQL_RELAY_LOG_basename, "MYSQL_RELAY_LOG::basename", 0},
  { &key_memory_MYSQL_RELAY_LOG_index, "MYSQL_RELAY_LOG::index", 0},
  { &key_memory_rpl_filter, "rpl_filter memory", 0},
  { &key_memory_errmsgs, "errmsgs", 0},
  { &key_memory_Gis_read_stream_err_msg, "Gis_read_stream::err_msg", 0},
//  { &key_memory_Geometry_objects_data, "Geometry::ptr_and_wkb_data", 0},
  { &key_memory_MYSQL_LOCK, "MYSQL_LOCK", 0},
//  { &key_memory_NET_buff, "NET::buff", 0},
//  { &key_memory_NET_compress_packet, "NET::compress_packet", 0},
  { &key_memory_Event_scheduler_scheduler_param, "Event_scheduler::scheduler_param", 0},
//  { &key_memory_Gtid_set_Interval_chunk, "Gtid_set::Interval_chunk", 0},
//  { &key_memory_Owned_gtids_sidno_to_hash, "Owned_gtids::sidno_to_hash", 0},
//  { &key_memory_Sid_map_Node, "Sid_map::Node", 0},
//  { &key_memory_Gtid_state_group_commit_sidno, "Gtid_state::group_commit_sidno_locks", 0},
//  { &key_memory_Mutex_cond_array_Mutex_cond, "Mutex_cond_array::Mutex_cond", 0},
  { &key_memory_TABLE_RULE_ENT, "TABLE_RULE_ENT", 0},
//  { &key_memory_Rpl_info_table, "Rpl_info_table", 0},
  { &key_memory_Rpl_info_file_buffer, "Rpl_info_file::buffer", 0},
//  { &key_memory_db_worker_hash_entry, "db_worker_hash_entry", 0},
//  { &key_memory_rpl_slave_check_temp_dir, "rpl_slave::check_temp_dir", 0},
//  { &key_memory_rpl_slave_command_buffer, "rpl_slave::command_buffer", 0},
  { &key_memory_binlog_ver_1_event, "binlog_ver_1_event", 0},
  { &key_memory_SLAVE_INFO, "SLAVE_INFO", 0},
  { &key_memory_binlog_pos, "binlog_pos", 0},
//  { &key_memory_HASH_ROW_ENTRY, "HASH_ROW_ENTRY", 0},
  { &key_memory_binlog_statement_buffer, "binlog_statement_buffer", 0},
//  { &key_memory_partition_syntax_buffer, "partition_syntax_buffer", 0},
//  { &key_memory_READ_INFO, "READ_INFO", 0},
  { &key_memory_JOIN_CACHE, "JOIN_CACHE", 0},
//  { &key_memory_TABLE_sort_io_cache, "TABLE::sort_io_cache", 0},
//  { &key_memory_frm, "frm", 0},
  { &key_memory_Unique_sort_buffer, "Unique::sort_buffer", 0},
  { &key_memory_Unique_merge_buffer, "Unique::merge_buffer", 0},
  { &key_memory_TABLE, "TABLE", PSI_FLAG_GLOBAL}, /* Table cache */
//  { &key_memory_frm_extra_segment_buff, "frm::extra_segment_buff", 0},
//  { &key_memory_frm_form_pos, "frm::form_pos", 0},
  { &key_memory_frm_string, "frm::string", 0},
//  { &key_memory_LOG_name, "LOG_name", 0},
  { &key_memory_DATE_TIME_FORMAT, "DATE_TIME_FORMAT", 0},
  { &key_memory_DDL_LOG_MEMORY_ENTRY, "DDL_LOG_MEMORY_ENTRY", 0},
  { &key_memory_ST_SCHEMA_TABLE, "ST_SCHEMA_TABLE", 0},
  { &key_memory_ignored_db, "ignored_db", 0},
  { &key_memory_PROFILE, "PROFILE", 0},
  { &key_memory_global_system_variables, "global_system_variables", 0},
  { &key_memory_THD_variables, "THD::variables", 0},
//  { &key_memory_Security_context, "Security_context", 0},
//  { &key_memory_shared_memory_name, "Shared_memory_name", 0},
  { &key_memory_bison_stack, "bison_stack", 0},
  { &key_memory_THD_handler_tables_hash, "THD::handler_tables_hash", 0},
  { &key_memory_hash_index_key_buffer, "hash_index_key_buffer", 0},
  { &key_memory_dboptions_hash, "dboptions_hash", 0},
  { &key_memory_dbnames_cache, "dbnames_cache", 0},
  { &key_memory_user_conn, "user_conn", 0},
//  { &key_memory_LOG_POS_COORD, "LOG_POS_COORD", 0},
//  { &key_memory_XID_STATE, "XID_STATE", 0},
  { &key_memory_MPVIO_EXT_auth_info, "MPVIO_EXT::auth_info", 0},
//  { &key_memory_opt_bin_logname, "opt_bin_logname", 0},
  { &key_memory_Query_cache, "Query_cache", PSI_FLAG_GLOBAL},
//  { &key_memory_READ_RECORD_cache, "READ_RECORD_cache", 0},
//  { &key_memory_Quick_ranges, "Quick_ranges", 0},
//  { &key_memory_File_query_log_name, "File_query_log::name", 0},
  { &key_memory_Table_trigger_dispatcher, "Table_trigger_dispatcher::m_mem_root", 0},
//  { &key_memory_thd_timer, "thd_timer", 0},
//  { &key_memory_THD_Session_tracker, "THD::Session_tracker", 0},
//  { &key_memory_THD_Session_sysvar_resource_manager, "THD::Session_sysvar_resource_manager", 0},
//  { &key_memory_show_slave_status_io_gtid_set, "show_slave_status_io_gtid_set", 0},
//  { &key_memory_write_set_extraction, "write_set_extraction", 0},
//  { &key_memory_get_all_tables, "get_all_tables", 0},
//  { &key_memory_fill_schema_schemata, "fill_schema_schemata", 0},
  { &key_memory_native_functions, "native_functions", PSI_FLAG_GLOBAL},
};

/**
  Initialise all the performance schema instrumentation points
  used by the server.
*/
void init_server_psi_keys(void)
{
  const char* category= "sql";
  int count;

  count= array_elements(all_server_mutexes);
  mysql_mutex_register(category, all_server_mutexes, count);

  count= array_elements(all_server_rwlocks);
  mysql_rwlock_register(category, all_server_rwlocks, count);

  count= array_elements(all_server_conds);
  mysql_cond_register(category, all_server_conds, count);

  count= array_elements(all_server_threads);
  mysql_thread_register(category, all_server_threads, count);

  count= array_elements(all_server_files);
  mysql_file_register(category, all_server_files, count);

  count= array_elements(all_server_stages);
  mysql_stage_register(category, all_server_stages, count);

  count= array_elements(all_server_sockets);
  mysql_socket_register(category, all_server_sockets, count);

  count= array_elements(all_server_memory);
  mysql_memory_register(category, all_server_memory, count);

#ifdef HAVE_PSI_STATEMENT_INTERFACE
  init_sql_statement_info();
  count= array_elements(sql_statement_info);
  mysql_statement_register(category, sql_statement_info, count);

  init_sp_psi_keys();

  category= "com";
  init_com_statement_info();

  /*
    Register [0 .. COM_QUERY - 1] as "statement/com/..."
  */
  count= (int) COM_QUERY;
  mysql_statement_register(category, com_statement_info, count);

  /*
    Register [COM_QUERY + 1 .. COM_END] as "statement/com/..."
  */
  count= (int) COM_END - (int) COM_QUERY;
  mysql_statement_register(category, & com_statement_info[(int) COM_QUERY + 1], count);

  category= "abstract";
  /*
    Register [COM_QUERY] as "statement/abstract/com_query"
  */
  mysql_statement_register(category, & com_statement_info[(int) COM_QUERY], 1);

  /*
    When a new packet is received,
    it is instrumented as "statement/abstract/new_packet".
    Based on the packet type found, it later mutates to the
    proper narrow type, for example
    "statement/abstract/query" or "statement/com/ping".
    In cases of "statement/abstract/query", SQL queries are given to
    the parser, which mutates the statement type to an even more
    narrow classification, for example "statement/sql/select".
  */
  stmt_info_new_packet.m_key= 0;
  stmt_info_new_packet.m_name= "new_packet";
  stmt_info_new_packet.m_flags= PSI_FLAG_MUTABLE;
  mysql_statement_register(category, &stmt_info_new_packet, 1);

  /*
    Statements processed from the relay log are initially instrumented as
    "statement/abstract/relay_log". The parser will mutate the statement type to
    a more specific classification, for example "statement/sql/insert".
  */
  stmt_info_rpl.m_key= 0;
  stmt_info_rpl.m_name= "relay_log";
  stmt_info_rpl.m_flags= PSI_FLAG_MUTABLE;
  mysql_statement_register(category, &stmt_info_rpl, 1);
#endif
}

#endif /* HAVE_PSI_INTERFACE */


/*
  Connection ID allocation.

  We need to maintain thread_ids in the 32bit range,
  because this is how it is passed to the client in the protocol.

  The idea is to maintain a id range, initially set to
 (0,UINT32_MAX). Whenever new id is needed, we increment the
  lower limit and return its new value.

  On "overflow", if id can not be generated anymore(i.e lower == upper -1),
  we recalculate the range boundaries.
  To do that, we first collect thread ids that are in use, by traversing
  THD list, and find largest region within (0,UINT32_MAX), that is still free.

*/

static my_thread_id thread_id_max= UINT_MAX32;

#include <vector>
#include <algorithm>

/*
  Find largest unused thread_id range.

  i.e for every number N within the returned range,
  there is no existing connection with thread_id equal to N.

  The range is exclusive, lower bound is always >=0 and
  upper bound <=MAX_UINT32.

  @param[out] low  - lower bound for the range
  @param[out] high - upper bound for the range
*/

static my_bool recalculate_callback(THD *thd, std::vector<my_thread_id> *ids)
{
  ids->push_back(thd->thread_id);
  return 0;
}


static void recalculate_thread_id_range(my_thread_id *low, my_thread_id *high)
{
  std::vector<my_thread_id> ids;

  // Add sentinels
  ids.push_back(0);
  ids.push_back(UINT_MAX32);
  server_threads.iterate(recalculate_callback, &ids);

  std::sort(ids.begin(), ids.end());
  my_thread_id max_gap= 0;
  for (size_t i= 0; i < ids.size() - 1; i++)
  {
    my_thread_id gap= ids[i+1] - ids[i];
    if (gap > max_gap)
    {
      *low= ids[i];
      *high= ids[i+1];
      max_gap= gap;
    }
  }

  if (max_gap < 2)
  {
    /* Can't find free id. This is not really possible,
      we'd need 2^32 connections for this to happen.*/
    sql_print_error("Cannot find free connection id.");
    abort();
  }
}


my_thread_id next_thread_id(void)
{
  my_thread_id retval;
  DBUG_EXECUTE_IF("thread_id_overflow", global_thread_id= thread_id_max-2;);

  mysql_mutex_lock(&LOCK_thread_id);

  if (unlikely(global_thread_id == thread_id_max - 1))
  {
    recalculate_thread_id_range(&global_thread_id, &thread_id_max);
  }

  retval= ++global_thread_id;

  mysql_mutex_unlock(&LOCK_thread_id);
  return retval;
}
