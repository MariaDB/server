#include <my_global.h>
#include <m_string.h>
extern "C" {
#include "maria_def.h"
}
#undef LSN_MAX
#include "aria_backup_client.h"
#include "backup_copy.h"
#include "common.h"
#include "sql_table.h"
#include "ma_checkpoint.h"
#include "ma_recovery.h"
#include "backup_debug.h"
#include "aria_backup.h"
#include <thread>
#include <string>
#include <vector>
#include <memory>
#include <limits>
#include <unordered_map>
#include <atomic>
#include <utility>
#include <sstream>
#include <iomanip>
#include <cstdlib>

namespace aria {

const char *log_preffix = "aria_log.";


static std::string log_file_name_only(size_t log_num) {
	std::string log_file;
	{
		std::stringstream ss;
		ss << std::setw(8) << std::setfill('0') << log_num;
		log_file.append(log_preffix).append(ss.str());
	}
	return log_file;
}


static std::string log_file_name(const char *datadir_path, size_t log_num) {
	std::string log_file(datadir_path);
	return log_file.append("/").append(log_file_name_only(log_num));
}


class LogFileCollection
{
  uint32 m_first;
  uint32 m_count;
public:
  uint32 first() const { return m_first; }
  uint32 count() const { return m_count; }
  uint32 last() const
  {
    DBUG_ASSERT(m_count > 0);
    return m_first + m_count - 1;
  }

  // Initialize by checking existing log files on the disk
  LogFileCollection(const char *datadir, uint32 max_log_no)
  {
    uint32 end= find_greatest_existing_log(datadir, max_log_no);
    if (!end)
    {
      // No log files were found at all
      m_first= 0;
      m_count= 0;
    }
    else if (end == 1)
    {
      // Just the very first one log file (aria_log.00000001) was found.
      m_first= 1;
      m_count= 1;
    }
    else
    {
      // Multiple files were found
      m_first= find_greatest_missing_log(datadir, end - 1) + 1;
      m_count= 1 + end - m_first;
    }
  }

  /*
    Skip all missing log files and find the greatest existing log file, or
    Skip all existing log files and find the greatest missing log file.

    @param datadir  - Search files in this directory
    @param start    - Start searching from this log number and go downto 1.
    @param kind     - true  - search for an existing file
                      false - search for a missing file.
    @returns        - [1..start] - the greatest found log file
                                   of the searched kind
                    - 0  - if no log files of this kind
                           were found in the range [1..start].
  */
  static uint32 find_greatest_existing_or_missing_log(const char *datadir,
                                                      uint32 start,
                                                      bool kind)
  {
    DBUG_ASSERT(start > 0);
    for (uint32 i= start; i > 0; i--)
    {
      if (file_exists(log_file_name(datadir, i).c_str()) == kind)
        return i;
    }
    return 0; // No log files of the searched kind were found
  }

  static uint32 find_greatest_existing_log(const char *datadir, uint32 start)
  {
    return find_greatest_existing_or_missing_log(datadir, start, true);
  }

  static uint32 find_greatest_missing_log(const char *datadir, uint32 start)
  {
    return find_greatest_existing_or_missing_log(datadir, start, false);
  }

  /*
    In some scenarios (e.g. log rotate) some new log files can appear
    outside of the initially assumed [first,last] log number range.
    This function adds all extra files behind "last".
  */
  void find_logs_after_last(const char *datadir)
  {
    DBUG_ASSERT(m_count > 0);
    for ( ;
         file_exists(log_file_name(datadir, last() + 1).c_str()) ;
         m_count++)
    {  }
  }

  void report_found(unsigned thread_num) const
  {
    if (m_count)
      msg(thread_num,
          "Found %u aria log files, "
          "minimum log number %u, "
          "maximum log number %u",
          m_count, m_first, last());
  }

  void die_if_missing(uint32 logno) const
  {
    DBUG_ASSERT(logno > 0);
    if (!m_count || m_first > logno || last() < logno)
      die("Aria log file %u does not exists.", logno);
  }
};


class Table {
public:
	struct Partition {
		std::string m_file_path;
		File m_index_file = -1;
		MY_STAT m_index_file_stat;
		File m_data_file = -1;
		MY_STAT m_data_file_stat;
	};
	Table() = default;
	Table (Table &&other) = delete;
	Table &  operator= (Table &&other) = delete;
	Table(const Table &) = delete;
	Table & operator= (const Table &) = delete;
	~Table();
	bool init(const char *data_file_path);
	bool open(MYSQL *con, bool opt_no_lock, unsigned thread_num);
	bool close();
	bool copy(ds_ctxt_t *ds, unsigned thread_num);

	bool is_online_backup_safe() const {
		DBUG_ASSERT(is_opened());
		return m_cap.online_backup_safe;
	}
	bool is_stats() const {
		return is_stats_table(m_db.c_str(), m_table.c_str());
	}
	bool is_log() const {
		return is_log_table(m_db.c_str(), m_table.c_str());
	}
	bool is_opened() const {
		return !m_partitions.empty() &&
			m_partitions[0].m_index_file >= 0 && m_partitions[0].m_data_file >= 0;
	};
	std::string &get_full_name() {
		return m_full_name;
	}
	std::string &get_db() { return m_db; }
	std::string &get_table() { return m_table; }
	std::string &get_version() { return m_table_version; }
	bool is_partitioned() const { return m_partitioned; }
	void add_partition(const Table &partition) {
		DBUG_ASSERT(is_partitioned());
		m_partitions.push_back(partition.m_partitions[0]);
	}
#ifndef DBUG_OFF
	const std::string& get_sql_name() const { return m_sql_name; }
#endif //DBUG_OFF
private:

	bool copy(ds_ctxt_t *ds, bool is_index, unsigned thread_num);
	// frm and par files will be copied under BLOCK_DDL stage in
	// backup_copy_non_system()
	bool copy_frm_and_par(ds_ctxt_t *ds, unsigned thread_num);
	bool read_table_version_id(File file);

	std::string m_db;
	std::string m_table;
	std::string m_full_name;
	std::string m_frm_par_path;
	std::string m_table_version;
#ifndef DBUG_OFF
	std::string m_sql_name;
#endif //DBUG_OFF
	bool m_partitioned = false;
	std::vector<Partition> m_partitions;
	ARIA_TABLE_CAPABILITIES m_cap;
};

Table::~Table() {
	(void)close();
}

bool Table::init(const char *data_file_path) {
		DBUG_ASSERT(data_file_path);

		const char *ext_pos = strrchr(data_file_path, '.');
		if (!ext_pos)
			return false;

		char db_name_orig[FN_REFLEN];
		char table_name_orig[FN_REFLEN];
		parse_db_table_from_file_path(
			data_file_path, db_name_orig, table_name_orig);
		if (!db_name_orig[0] || !table_name_orig[0])
			return false;
		char db_name_conv[FN_REFLEN];
		char table_name_conv[FN_REFLEN];
		filename_to_tablename(db_name_orig, db_name_conv, sizeof(db_name_conv));
		filename_to_tablename(
			table_name_orig, table_name_conv, sizeof(table_name_conv));
		if (!db_name_conv[0] || !table_name_conv[0])
			return false;

		if (strstr(data_file_path, "#P#"))
			m_partitioned = true;

		const char *table_name_begin = strrchr(data_file_path, FN_LIBCHAR);
		if (!table_name_begin)
			return false;
		m_frm_par_path.assign(data_file_path, table_name_begin + 1).
			append(table_name_orig);

		m_db.assign(db_name_conv);
		m_table.assign(table_name_conv);
		// TODO: find the correct way to represent quoted table/db names
		m_full_name.assign("`").append(m_db).append("`.`").
			append(m_table).append("`");
#ifndef DBUG_OFF
		m_sql_name.assign(m_db).append("/").append(m_table);
#endif // DBUG_OFF
		Partition partition;
		partition.m_file_path.assign(data_file_path, ext_pos - data_file_path);
		m_partitions.push_back(std::move(partition));
		return true;
}

bool Table::read_table_version_id(File file) {
	m_table_version = ::read_table_version_id(file);
	return m_table_version.empty();
}

bool Table::open(MYSQL *con, bool opt_no_lock, unsigned thread_num) {
	int error= 1;
	bool have_capabilities = false;
	File frm_file = -1;

	if (!opt_no_lock && !backup_lock(con, m_full_name.c_str())) {
		msg(thread_num, "Error on BACKUP LOCK for aria table %s",
			m_full_name.c_str());
		goto exit;
	}

	for (Partition &partition : m_partitions) {
		std::string file_path = partition.m_file_path + ".MAI";
		if ((partition.m_index_file= my_open(file_path.c_str(),
						O_RDONLY | O_SHARE | O_NOFOLLOW | O_CLOEXEC,
						MYF(MY_WME))) < 0) {
			msg(thread_num, "Error on aria table file open %s", file_path.c_str());
			goto exit;
		}
		if (!my_stat(file_path.c_str(), &partition.m_index_file_stat, MYF(0))) {
			msg(thread_num, "Error on aria table file stat %s", file_path.c_str());
			goto exit;
		}
		if (!have_capabilities) {
			if ((error= aria_get_capabilities(partition.m_index_file, &m_cap))) {
				msg(thread_num, "aria_get_capabilities failed: %d", error);
				goto exit;
			}
			have_capabilities = true;
		}

		file_path = partition.m_file_path + ".MAD";
		if ((partition.m_data_file= my_open(file_path.c_str(),
						O_RDONLY | O_SHARE | O_NOFOLLOW | O_CLOEXEC, MYF(MY_WME))) < 0) {
			msg(thread_num, "Error on aria table file open %s", file_path.c_str());
			goto exit;
		}
		if (!my_stat(file_path.c_str(), &partition.m_data_file_stat, MYF(0))) {
			msg(thread_num, "Error on aria table file stat %s", file_path.c_str());
			goto exit;
		}
	}

	if ((frm_file = mysql_file_open(
		key_file_frm, (m_frm_par_path + ".frm").c_str(),
		O_RDONLY | O_SHARE, MYF(0))) < 0) {
		msg(thread_num, "Error on aria table %s file open",
			(m_frm_par_path + ".frm").c_str());
		goto exit;
	}

	error = 0;

exit:
	if (!opt_no_lock && !backup_unlock(con)) {
		msg(thread_num, "Error on BACKUP UNLOCK for aria table %s",
			m_full_name.c_str());
		error = 1;
	}
	if (error)
		(void)close();
	else {
		(void)read_table_version_id(frm_file);
		mysql_file_close(frm_file, MYF(MY_WME));
	}
	return !error;
}

bool Table::close() {
	for (Partition &partition : m_partitions) {
		if (partition.m_index_file >= 0) {
			my_close(partition.m_index_file, MYF(MY_WME));
			partition.m_index_file = -1;
		}
		if (partition.m_data_file >= 0) {
			my_close(partition.m_data_file, MYF(MY_WME));
			partition.m_data_file = -1;
		}
	}
	return true;
}

bool Table::copy(ds_ctxt_t *ds, unsigned thread_num) {
	DBUG_ASSERT(is_opened());
	DBUG_MARIABACKUP_EVENT_LOCK("before_aria_table_copy",
          fil_space_t::name_type(m_sql_name.data(), m_sql_name.size()));
	bool result =
//		copy_frm_and_par(ds, thread_num) &&
			copy(ds, true, thread_num) && copy(ds, false, thread_num);
	return result;
}

bool Table::copy(ds_ctxt_t *ds, bool is_index, unsigned thread_num) {
	DBUG_ASSERT(ds);
	const char *ext = is_index ? ".MAI" : ".MAD";
	int error= 1;
	for (const Partition &partition : m_partitions) {
		ds_file_t		*dst_file = nullptr;
		uchar *copy_buffer = nullptr;
		std::string full_name = partition.m_file_path + ext;
		const char	*dst_path =
			(xtrabackup_copy_back || xtrabackup_move_back) ?
			full_name.c_str() : trim_dotslash(full_name.c_str());

		dst_file = ds_open(ds, dst_path,
			is_index ? &partition.m_index_file_stat : &partition.m_data_file_stat);
		if (!dst_file) {
			msg(thread_num, "error: cannot open the destination stream for %s",
				dst_path);
			goto err;
		}

		copy_buffer =
			reinterpret_cast<uchar *>(my_malloc(PSI_NOT_INSTRUMENTED,
			m_cap.block_size, MYF(0)));

		DBUG_MARIABACKUP_EVENT_LOCK(
			is_index ?
				"before_aria_index_file_copy":
				"before_aria_data_file_copy",
			fil_space_t::name_type(m_sql_name.data(),
                        m_sql_name.size()));

		for (ulonglong block= 0 ; ; block++) {
			size_t length = m_cap.block_size;
			if (is_index) {
				error= aria_read_index(partition.m_index_file,
						       &m_cap, block,
						       copy_buffer);
			} else {
				error= aria_read_data(partition.m_data_file,
						      &m_cap, block,
						      copy_buffer, &length);
			}

			if (error == HA_ERR_END_OF_FILE)
			  break;

			if (error) {
				msg(thread_num, "error: aria_read %s failed:  %d",
					is_index ? "index" : "data", error);
				goto err;
			}
			xtrabackup_io_throttling();
			if ((error = ds_write(dst_file, copy_buffer, length))) {
				msg(thread_num, "error: aria_write failed:  %d", error);
				goto err;
			}
		}

		DBUG_MARIABACKUP_EVENT_LOCK(
			is_index ?
				"after_aria_index_file_copy":
				"after_aria_data_file_copy",
			fil_space_t::name_type(m_sql_name.data(),
                        m_sql_name.size()));

		error = 0;
		msg(thread_num, "aria table file %s is copied successfully.",
			full_name.c_str());

	err:
		if (dst_file)
			ds_close(dst_file);
		if (copy_buffer)
			my_free(copy_buffer);
		if (error)
			break;
	}
	return !error;
}

class BackupImpl {
public:
	BackupImpl(
		const char *datadir_path,
		const char *aria_log_path,
		ds_ctxt_t *datasink, bool opt_no_lock,
		std::vector<MYSQL *> &con_pool, ThreadPool &thread_pool) :
		m_datadir_path(datadir_path),
		m_aria_log_dir_path(aria_log_path),
		m_ds(datasink), m_con_pool(con_pool),
		m_tasks_group(thread_pool), m_thread_pool(thread_pool) { }
	~BackupImpl() { destroy(); }
	bool init();
	bool start(bool no_lock);
	bool wait_for_finish();
	bool copy_offline_tables(
		const std::unordered_set<table_key_t> *exclude_tables, bool no_lock,
		bool copy_stats);
	bool finalize();
	void set_post_copy_table_hook(const post_copy_table_hook_t &hook) {
		m_table_post_copy_hook = hook;
	}
	bool copy_log_tail() { return copy_log_tail(0, false); }
private:
	void destroy();
	void scan_job(bool no_lock, unsigned thread_num);
	bool copy_log_tail(unsigned thread_num, bool finalize);
	void copy_log_file_job(size_t log_num, unsigned thread_num);
	void destroy_log_tail();
	void process_table_job(Table *table, bool online_only, bool copy_stats,
		bool no_lock, unsigned thread_num);

	const char *m_datadir_path;
	const char *m_aria_log_dir_path;
	std::string aria_log_dir_path() const
	{
	  if (!m_aria_log_dir_path || !m_aria_log_dir_path[0])
	    return m_datadir_path;
          if (is_absolute_path(m_aria_log_dir_path))
            return m_aria_log_dir_path;
          return std::string(m_datadir_path).append("/")
                                            .append(m_aria_log_dir_path);
	}
	ds_ctxt_t *m_ds;
	std::vector<MYSQL *> &m_con_pool;

	TasksGroup m_tasks_group;

	std::mutex m_offline_tables_mutex;
	std::vector<std::unique_ptr<Table>> m_offline_tables;
	post_copy_table_hook_t m_table_post_copy_hook;

	ThreadPool &m_thread_pool;

	size_t m_last_log_num = 0;
	ds_file_t*	m_last_log_dst = nullptr;
	File m_last_log_src = -1;
};

bool BackupImpl::init() {
	DBUG_ASSERT(m_tasks_group.is_finished());
	return true;
};

void BackupImpl::destroy() {
	DBUG_ASSERT(m_tasks_group.is_finished());
	destroy_log_tail();
}

bool BackupImpl::start(bool no_lock) {
	DBUG_ASSERT(m_tasks_group.is_finished());
	m_tasks_group.push_task(
		std::bind(&BackupImpl::scan_job, this, no_lock, std::placeholders::_1));
	return true;
}

void BackupImpl::process_table_job(
	Table *table_ptr, bool online_only, bool copy_stats, bool no_lock,
	unsigned thread_num) {
	DBUG_ASSERT(table_ptr);
	DBUG_ASSERT(thread_num < m_con_pool.size());
	std::unique_ptr<Table> table(table_ptr);
	bool is_online;
	bool is_stats;
	bool need_copy;
	int result = 1;

	if (!m_tasks_group.get_result())
		goto exit;

	if (!table->open(m_con_pool[thread_num], no_lock, thread_num)) {
		// if table can't be opened, it might be removed or renamed, this is not
		// error for transactional tables
		table->close(); // Close opened table files
		goto exit;
	}

	is_online = table->is_online_backup_safe();
	is_stats = table->is_stats();

	need_copy = (!online_only || is_online) && (copy_stats || !is_stats);

	if (need_copy && !table->copy(m_ds, thread_num)) {
		table->close();
		DBUG_MARIABACKUP_EVENT_LOCK("after_aria_table_copy",
			fil_space_t::name_type(table->get_sql_name().data(),
                                               table->get_sql_name().size()));
		// if table is opened, it must be copied,
		// the corresponding diagnostic messages must be issued in Table::copy()
		result = 0;
		goto exit;
	}

	if (!table->close()) {
		msg(thread_num, "Can't close aria table %s.\n",
			table->get_full_name().c_str());
		result = 0;
		goto exit;
	}

	if (!need_copy) {
		std::lock_guard<std::mutex> lock(m_offline_tables_mutex);
		m_offline_tables.push_back(std::move(table));
	}
	else {
		DBUG_MARIABACKUP_EVENT_LOCK("after_aria_table_copy",
			fil_space_t::name_type(table->get_sql_name().data(),
                                               table->get_sql_name().size()));
		if (m_table_post_copy_hook)
			m_table_post_copy_hook(
				std::move(table->get_db()),
				std::move(table->get_table()),
				std::move(table->get_version()));
	}
exit:
	m_tasks_group.finish_task(result);
}


void BackupImpl::scan_job(bool no_lock, unsigned thread_num) {
	std::unordered_map<std::string, std::unique_ptr<Table>> partitioned_tables;

	std::string aria_log_dir_path_cache(aria_log_dir_path());
	std::string log_control_file_path(aria_log_dir_path_cache);
	log_control_file_path.append("/aria_log_control");
	if (!m_ds->copy_file(
		log_control_file_path.c_str(), "aria_log_control",
		0, false)) {
		msg("Aria log control file copying error.");
		m_tasks_group.finish_task(0);
		return;
	}

	msg(thread_num, "Loading aria_log_control.");
	aria_readonly= 1;
	maria_data_root= aria_log_dir_path_cache.c_str();
	if (ma_control_file_open(FALSE, FALSE, FALSE, O_RDONLY))
		die("Can't open Aria control file (%d)", errno);
	uint32 aria_log_control_last_log_number= last_logno;
	msg(thread_num, "aria_log_control: last_log_number: %d",
	    aria_log_control_last_log_number);
	ma_control_file_end();

	msg(thread_num, "Start scanning aria tables.");

	foreach_file_in_db_dirs(m_datadir_path, [&](const char *file_path)->bool {

		if (check_if_skip_table(file_path)) {
			msg(thread_num, "Skipping %s.", file_path);
			return true;
		}

		if (!ends_with(file_path, ".MAD"))
			return true;

		std::unique_ptr<Table> table(new Table());
		if (!table->init(file_path)) {
			msg(thread_num, "Can't init aria table %s.\n", file_path);
			return true;
		}

		if (table->is_log())
			return true;

		if (table->is_partitioned()) {
			auto table_it = partitioned_tables.find(table->get_full_name());
			if (table_it == partitioned_tables.end()) {
				partitioned_tables[table->get_full_name()] = std::move(table);
			} else {
				table_it->second->add_partition(*table);
			}
			return true;
		}

		m_tasks_group.push_task(
			std::bind(&BackupImpl::process_table_job, this, table.release(), true,
					false, no_lock, std::placeholders::_1));
		return true;
	});

	for (auto &table_it : partitioned_tables) {
		m_tasks_group.push_task(
			std::bind(&BackupImpl::process_table_job, this, table_it.second.release(),
				true, false, no_lock, std::placeholders::_1));
	}

	msg(thread_num, "Start scanning aria log files.");

        LogFileCollection logs(aria_log_dir_path_cache.c_str(),
                               aria_log_control_last_log_number);
        logs.report_found(thread_num);
        logs.die_if_missing(aria_log_control_last_log_number);

        m_last_log_num= logs.last();

        DBUG_MARIABACKUP_EVENT("after_scanning_log_files", {});

        for (uint32 i= logs.first(); i <= logs.last(); ++i)
          m_tasks_group.push_task(
            std::bind(&BackupImpl::copy_log_file_job, this,
                      i, std::placeholders::_1));

	msg(thread_num, "Stop scanning aria tables.");

	m_tasks_group.finish_task(1);
}

template<typename T>
T align_down(T n, ulint align_no)
{
	DBUG_ASSERT(align_no > 0);
	DBUG_ASSERT(ut_is_2pow(align_no));
	return n & ~(static_cast<T>(align_no) - 1);
}

static ssize_t copy_file_chunk(File src, ds_file_t* dst, size_t size) {
	size_t bytes_read;
	static const size_t max_buf_size = 10 * 1024 * 1024;
	size_t buf_size = size ? std::min(size, max_buf_size) : max_buf_size;
	std::unique_ptr<uchar[]> buf(new uchar[buf_size]);
	ssize_t copied_size = 0;
	bool unlim = !size;
	while((unlim || size) && (bytes_read = my_read(src, buf.get(),
		unlim ? buf_size : std::min(buf_size, size), MY_WME))) {
		if (bytes_read == size_t(-1))
			return -1;
		xtrabackup_io_throttling();
		if (ds_write(dst, buf.get(), bytes_read))
			return -1;
		copied_size += bytes_read;
		if (!unlim)
			size -= bytes_read;
	}
	return copied_size;
}

bool BackupImpl::copy_log_tail(unsigned thread_num, bool finalize) {
	bool result = false;
	std::string log_file = log_file_name(aria_log_dir_path().c_str(), m_last_log_num);
	std::string prev_log_file;
	ssize_t total_bytes_copied = 0;
	MY_STAT stat_info;
	my_off_t file_offset = 0;
	size_t to_copy_size = 0;

repeat:
	memset(&stat_info, 0, sizeof(MY_STAT));
	if (!m_tasks_group.get_result()) {
		msg(thread_num, "Skip copying aria lof file tail %s due to error.",
			log_file.c_str());
		result = true;
		goto exit;
	}

	msg(thread_num, "Start copying aria log file tail: %s", log_file.c_str());

	if (m_last_log_src < 0 && (m_last_log_src =
		my_open(log_file.c_str(), O_RDONLY | O_SHARE | O_NOFOLLOW | O_CLOEXEC,
			MYF(MY_WME))) < 0) {
		msg("Aria log file %s open failed: %d", log_file.c_str(), my_errno);
		goto exit;
	}

	if (!m_last_log_dst &&
		!(m_last_log_dst = ds_open(m_ds,
		                           log_file_name_only(m_last_log_num).c_str(),
		                           &stat_info, false))) {
		msg(thread_num, "error: failed to open the target stream for "
			"aria log file %s.",
			log_file.c_str());
		goto exit;
	}

// If there is no need to finalize log file copying, calculate the size to copy
// without the last page, which can be rewritten by the server
// (see translog_force_current_buffer_to_finish()).
	if (!finalize) {
		if (my_fstat(m_last_log_src, &stat_info, MYF(0))) {
			msg(thread_num, "error: failed to get file size for aria log file: %s.",
					log_file.c_str());
			goto exit;
		}
		if ((file_offset = my_tell(m_last_log_src, MYF(0))) == (my_off_t)(-1)) {
			msg(thread_num, "error: failed to get file offset for aria log file: %s.",
					log_file.c_str());
			goto exit;
		}
		DBUG_ASSERT(file_offset <= static_cast<my_off_t>(stat_info.st_size));
		to_copy_size = static_cast<size_t>(stat_info.st_size - file_offset);
		to_copy_size = to_copy_size >= TRANSLOG_PAGE_SIZE ?
			(align_down(to_copy_size, TRANSLOG_PAGE_SIZE) - TRANSLOG_PAGE_SIZE) : 0;
	}

// Copy from the last position to the end of file,
// excluding the last page is there is no need to finalize the copy.
	if ((to_copy_size || finalize) &&
		(total_bytes_copied = copy_file_chunk(m_last_log_src,
		m_last_log_dst, to_copy_size)) < 0) {
			msg(thread_num, "Aria log file %s chunk copy error", log_file.c_str());
			goto exit;
	}

	msg(thread_num, "Stop copying aria log file tail: %s, copied %zu bytes",
		log_file.c_str(), total_bytes_copied);

// Check if there is new log file, if yes, then copy the last page of the old
// one, and fix it last LSN in the log header, as it is changed on new
// log file creating by the server (see translog_create_new_file() and
// translog_max_lsn_to_header()). Then close the old log file and repeat
// the copying for the new log file.
	prev_log_file = std::move(log_file);
	log_file = log_file_name(aria_log_dir_path().c_str(), m_last_log_num + 1);
	if (file_exists(log_file.c_str())) {
		uchar lsn_buff[LSN_STORE_SIZE];
		msg(thread_num, "Found new aria log tail file: %s, start copy %s tail",
			log_file.c_str(), prev_log_file.c_str());
		if ((total_bytes_copied = copy_file_chunk(m_last_log_src,
			m_last_log_dst, 0)) < 0) {
				msg(thread_num, "Aria log file %s tail copy error",
					prev_log_file.c_str());
				goto exit;
		}

		if (my_pread(m_last_log_src, lsn_buff, LSN_STORE_SIZE,
			(LOG_HEADER_DATA_SIZE - LSN_STORE_SIZE), MYF(0)) < LSN_STORE_SIZE) {
			msg(thread_num, "Aria lsn store read error for log file %s",
				prev_log_file.c_str());
			goto exit;
		}

		if (ds_seek_set(m_last_log_dst, (LOG_HEADER_DATA_SIZE - LSN_STORE_SIZE))) {
			msg(thread_num, "Set aria log pointer error for log file %s",
				prev_log_file.c_str());
			goto exit;
		}

		if (ds_write(m_last_log_dst, lsn_buff, LSN_STORE_SIZE)) {
			msg(thread_num, "LSN write error for aria log file %s",
				prev_log_file.c_str());
			goto exit;
		}

		msg(thread_num, "The last %zu bytes were copied for %s.",
			total_bytes_copied, prev_log_file.c_str());
		destroy_log_tail();
		++m_last_log_num;
		goto repeat;
	}

	result = true;

exit:
	if (!result)
		destroy_log_tail();
	return result;
}

void BackupImpl::copy_log_file_job(size_t log_num, unsigned thread_num) {
	DBUG_ASSERT(log_num <= m_last_log_num);

	if (!m_tasks_group.get_result()) {
		msg(thread_num, "Skip copying %zu aria log file due to error", log_num);
		m_tasks_group.finish_task(0);
		return;
	}

// Copy log file if the file is not the last one.
	if (log_num < m_last_log_num) {
		std::string log_file = log_file_name(aria_log_dir_path().c_str(), log_num);
		if (!m_ds->copy_file(log_file.c_str(),
  			log_file_name_only(log_num).c_str(),
			thread_num, false)) {
			msg(thread_num, "Error on copying %s aria log file.", log_file.c_str());
			m_tasks_group.finish_task(0);
		}
		else
			m_tasks_group.finish_task(1);
		return;
	}
// Copy the last log file.
	m_tasks_group.finish_task(copy_log_tail(thread_num, false) ? 1 : 0);
}

void BackupImpl::destroy_log_tail() {
	if (m_last_log_src >= 0) {
		my_close(m_last_log_src, MYF(MY_WME));
		m_last_log_src = -1;
	}
	if (m_last_log_dst) {
		ds_close(m_last_log_dst);
		m_last_log_dst = nullptr;
	}
}

bool BackupImpl::wait_for_finish() {
	return m_tasks_group.wait_for_finish();
}

bool BackupImpl::copy_offline_tables(
	const std::unordered_set<table_key_t> *exclude_tables, bool no_lock,
	bool copy_stats) {
	DBUG_ASSERT(m_tasks_group.is_finished());

	std::vector<std::unique_ptr<Table>> ignored_tables;

	while (true) {
		std::unique_lock<std::mutex> lock(m_offline_tables_mutex);
		if (m_offline_tables.empty())
			break;
		auto table = std::move(m_offline_tables.back());
		m_offline_tables.pop_back();
		lock.unlock();
		if ((exclude_tables &&
			exclude_tables->count(table_key(table->get_db(), table->get_table()))) ||
			(!copy_stats && table->is_stats())) {
			ignored_tables.push_back(std::move(table));
			continue;
		}
		m_tasks_group.push_task(
			std::bind(&BackupImpl::process_table_job, this, table.release(), false,
					copy_stats, no_lock, std::placeholders::_1));
	}

	if (!ignored_tables.empty()) {
		std::lock_guard<std::mutex> lock(m_offline_tables_mutex);
		m_offline_tables = std::move(ignored_tables);
	}

	return true;
}

bool BackupImpl::finalize() {
	DBUG_ASSERT(m_tasks_group.is_finished());
	DBUG_ASSERT(!m_con_pool.empty());
	bool result = true;
	msg("Start copying statistics aria tables.");
	copy_offline_tables(nullptr, true, true);
	while (!m_tasks_group.is_finished())
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	msg("Stop copying statistics aria tables.");
	copy_log_tail(0, true);
	destroy_log_tail();
	return result;
}

Backup::Backup(const char *datadir_path,
		const char *aria_log_path,
               ds_ctxt_t *datasink,
	 std::vector<MYSQL *> &con_pool, ThreadPool &thread_pool) :
		m_backup_impl(
			new BackupImpl(datadir_path, aria_log_path,
			               datasink, opt_no_lock, con_pool,
				thread_pool)) { }

Backup::~Backup() {
	delete m_backup_impl;
}

bool Backup::init() {
	return m_backup_impl->init();
}

bool Backup::start(bool no_lock) {
	return m_backup_impl->start(no_lock);
}

bool Backup::wait_for_finish() {
	return m_backup_impl->wait_for_finish();
}

bool Backup::copy_offline_tables(
	const std::unordered_set<table_key_t> *exclude_tables, bool no_lock,
	bool copy_stats) {
	return m_backup_impl->copy_offline_tables(exclude_tables, no_lock,
		copy_stats);
}

bool Backup::finalize() {
	return m_backup_impl->finalize();
}

bool Backup::copy_log_tail() {
	return m_backup_impl->copy_log_tail();
}

void Backup::set_post_copy_table_hook(const post_copy_table_hook_t &hook) {
	m_backup_impl->set_post_copy_table_hook(hook);
}

bool prepare(const char *target_dir) {
	maria_data_root= (char *)target_dir;
        maria_tmpdir= &mysql_tmpdir_list;

	if (maria_init())
		die("Can't init Aria engine (%d)", errno);

	maria_block_size= 0;                          /* Use block size from file */
		/* we don't want to create a control file, it MUST exist */
	if (ma_control_file_open(FALSE, TRUE, TRUE, control_file_open_flags))
		die("Can't open Aria control file (%d)", errno);

	if (last_logno == FILENO_IMPOSSIBLE)
		die("Can't find any Aria log");

        LogFileCollection logs(target_dir, last_logno);
        logs.die_if_missing(last_logno); // Fatal, a broken backup.
        /*
          "mariadb-backup --backup" can put extra log files,
          with log number greater than last_logno. For example,
          this combination of files is possible:
          - aria_log_control  (with last_logno==1)
          - aria_log.00000001 (last_logno)
          - aria_log.00000002 (last_logno+1, the extra log file)
          This can happen if during the ealier run of
          "mariadb-backup --backup" a log rotate happened.
          The extra log file is copied to the backup directory,
          but last_logno in aria_log_control does not get updated.
          This mismatch is probably not good and should eventually be fixed.
          But during "mariadb-backup --prepare" this mismatch goes away:
          aria_log_control gets fixed to say last_logno==2.
          See mysql-test/suite/mariabackup/aria_log_rotate_during_backup.test,
          it covers the scenario with one extra file created during --backup.
        */
        logs.find_logs_after_last(target_dir);
        last_logno= logs.last(); // Update last_logno if extra logs were found

	if (init_pagecache(maria_pagecache, 1024L*1024L, 0, 0,
		static_cast<uint>(maria_block_size), 0, MY_WME) == 0)
		die("Got error in Aria init_pagecache() (errno: %d)", errno);

	if (init_pagecache(maria_log_pagecache, 1024L*1024L,
		0, 0, TRANSLOG_PAGE_SIZE, 0, MY_WME) == 0 ||
		translog_init(maria_data_root, TRANSLOG_FILE_SIZE,
		0, 0, maria_log_pagecache, TRANSLOG_DEFAULT_FLAGS, FALSE))
		die("Can't init Aria loghandler (%d)", errno);

	if (maria_recovery_from_log())
		die("Aria log apply FAILED");

	if (maria_recovery_changed_data || recovery_failures) {
		if (ma_control_file_write_and_force(last_checkpoint_lsn, last_logno,
			max_trid_in_control_file, 0))
			die("Aria control file update error");
// TODO: find out do we need checkpoint here
	}

	maria_end();
	return true;
}

} // namespace aria
