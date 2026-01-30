#include "common_engine.h"
#include "backup_copy.h"
#include "xtrabackup.h"
#include "common.h"
#include "backup_debug.h"

#include <unordered_map>
#include <atomic>
#include <memory>
#include <chrono>

#include "innodb_binlog.h"


namespace common_engine {

class Table {
public:
	Table(std::string &db, std::string &table, std::string &fs_name) :
		m_db(std::move(db)), m_table(std::move(table)),
		m_fs_name(std::move(fs_name)) {}
	virtual ~Table() {}
	void add_file_name(const char *file_name) { m_fnames.push_back(file_name); }
	virtual bool copy(ds_ctxt_t *ds, MYSQL *con, bool no_lock,
		bool finalize, unsigned thread_num);
	std::string &get_db() { return m_db; }
	std::string &get_table() { return m_table; }
	std::string &get_version() { return m_version; }

protected:
	std::string m_db;
	std::string m_table;
	std::string m_fs_name;
	std::string m_version;
	std::vector<std::string> m_fnames;
};

bool
Table::copy(ds_ctxt_t *ds, MYSQL *con, bool no_lock, bool, unsigned thread_num) {
	static const size_t buf_size = 10 * 1024 * 1024;
	std::unique_ptr<uchar[]> buf;
	bool result = false;
	File frm_file = -1;
	std::vector<File> files;
	bool locked = false;
	std::string full_tname("`");
	full_tname.append(m_db).append("`.`").append(m_table).append("`");

	if (!no_lock && !backup_lock(con, full_tname.c_str())) {
		msg(thread_num, "Error on executing BACKUP LOCK for table %s",
			full_tname.c_str());
		goto exit;
	}
	else
		locked = !no_lock;

	if ((frm_file = mysql_file_open(key_file_frm, (m_fs_name + ".frm").c_str(),
		O_RDONLY | O_SHARE, MYF(0))) < 0 && !m_fnames.empty() &&
		!ends_with(m_fnames[0].c_str(), ".ARZ") &&
		!ends_with(m_fnames[0].c_str(), ".ARM")) {
		// Don't treat it as error, as the table can be dropped after it
		// was added to queue for copying
		result = true;
		goto exit;
	}

	for (const auto &fname : m_fnames) {
		File file = mysql_file_open(0, fname.c_str(),O_RDONLY | O_SHARE, MYF(0));
		if (file < 0) {
			char buf[MYSYS_STRERROR_SIZE];
			msg(thread_num, "Error %i on file %s open during %s table copy: %s",
			    errno, fname.c_str(), full_tname.c_str(),
			    my_strerror(buf, sizeof(buf), errno));
			goto exit;
		}
		files.push_back(file);
	}

	if (locked && !backup_unlock(con)) {
		msg(thread_num, "Error on BACKUP UNLOCK for table %s", full_tname.c_str());
		locked = false;
		goto exit;
	}

	locked = false;

	buf.reset(new uchar[buf_size]);

	for (size_t i = 0; i < m_fnames.size(); ++i) {
		ds_file_t *dst_file = nullptr;
		size_t bytes_read;
		size_t copied_size = 0;
		MY_STAT stat_info;

		if (my_fstat(files[i], &stat_info, MYF(0))) {
			msg(thread_num, "error: failed to get stat info for file %s of "
					"table %s", m_fnames[i].c_str(), full_tname.c_str());
			goto exit;
		}

		const char	*dst_path =
			(xtrabackup_copy_back || xtrabackup_move_back) ?
			m_fnames[i].c_str() : trim_dotslash(m_fnames[i].c_str());

		dst_file = ds_open(ds, dst_path, &stat_info, false);
		if (!dst_file) {
			msg(thread_num, "error: cannot open destination stream for %s, table %s",
				dst_path, full_tname.c_str());
			goto exit;
		}

		while ((bytes_read = my_read(files[i], buf.get(), buf_size, MY_WME))) {
			if (bytes_read == size_t(-1)) {
				msg(thread_num, "error: file %s read for table %s",
					m_fnames[i].c_str(), full_tname.c_str());
				ds_close(dst_file);
				goto exit;
			}
			xtrabackup_io_throttling();
			if (ds_write(dst_file, buf.get(), bytes_read)) {
				msg(thread_num, "error: file %s write for table %s",
					dst_path, full_tname.c_str());
				ds_close(dst_file);
				goto exit;
			}
			copied_size += bytes_read;
		}
		mysql_file_close(files[i], MYF(MY_WME));
		files[i] = -1;
		ds_close(dst_file);
		msg(thread_num, "Copied file %s for table %s, %zu bytes",
			m_fnames[i].c_str(), full_tname.c_str(), copied_size);
	}

	result = true;

#ifndef DBUG_OFF
	{
		std::string sql_name(m_db);
		sql_name.append("/").append(m_table);
		DBUG_MARIABACKUP_EVENT_LOCK("after_ce_table_copy", fil_space_t::name_type(sql_name.data(), sql_name.size()));
	}
#endif // DBUG_OFF
exit:
	if (frm_file >= 0) {
		m_version = ::read_table_version_id(frm_file);
		mysql_file_close(frm_file, MYF(MY_WME));
	}
	if (locked && !backup_unlock(con)) {
		msg(thread_num, "Error on BACKUP UNLOCK for table %s", full_tname.c_str());
		result = false;
	}
	for (auto file : files)
		if (file >= 0)
			mysql_file_close(file, MYF(MY_WME));
	return result;
}

// Append-only tables
class LogTable : public Table {
	public:
		LogTable(std::string &db, std::string &table, std::string &fs_name) :
			Table(db, table, fs_name) {}

		virtual ~LogTable() { (void)close(); }
		bool
			copy(ds_ctxt_t *ds, MYSQL *con, bool no_lock, bool finalize,
			unsigned thread_num) override;
		bool close();
	private:
		bool open(ds_ctxt_t *ds, unsigned thread_num);
		std::vector<File> m_src;
		std::vector<ds_file_t *> m_dst;
};

bool
LogTable::open(ds_ctxt_t *ds, unsigned thread_num) {
	DBUG_ASSERT(m_src.empty());
	DBUG_ASSERT(m_dst.empty());

	std::string full_tname("`");
	full_tname.append(m_db).append("`.`").append(m_table).append("`");

	for (const auto &fname : m_fnames) {
		File file = mysql_file_open(0, fname.c_str(),O_RDONLY | O_SHARE, MYF(0));
		if (file < 0) {
			msg(thread_num, "Error on file %s open during %s log table copy",
				fname.c_str(), full_tname.c_str());
			return false;
		}
		m_src.push_back(file);

		MY_STAT stat_info;
		if (my_fstat(file, &stat_info, MYF(0))) {
			msg(thread_num, "error: failed to get stat info for file %s of "
					"log table %s", fname.c_str(), full_tname.c_str());
			return false;
		}
		const char	*dst_path =
			(xtrabackup_copy_back || xtrabackup_move_back) ?
			fname.c_str() : trim_dotslash(fname.c_str());
		ds_file_t *dst_file = ds_open(ds, dst_path, &stat_info, false);
		if (!dst_file) {
			msg(thread_num, "error: cannot open destination stream for %s, "
				"log table %s", dst_path, full_tname.c_str());
			return false;
		}
		m_dst.push_back(dst_file);
	}

	File frm_file;
	if ((frm_file = mysql_file_open(key_file_frm, (m_fs_name + ".frm").c_str(),
		O_RDONLY | O_SHARE, MYF(0))) < 0 && !m_fnames.empty() &&
		!ends_with(m_fnames[0].c_str(), ".ARZ") &&
		!ends_with(m_fnames[0].c_str(), ".ARM")) {
		msg(thread_num, "Error on .frm file open for log table %s",
			full_tname.c_str());
		return false;
	}

	m_version = ::read_table_version_id(frm_file);
	mysql_file_close(frm_file, MYF(MY_WME));

	return true;
}

bool LogTable::close() {
	while (!m_src.empty()) {
		auto f = m_src.back();
		m_src.pop_back();
		mysql_file_close(f, MYF(MY_WME));
	}
	while (!m_dst.empty()) {
		auto f = m_dst.back();
		m_dst.pop_back();
		ds_close(f);
	}
	return true;
}

bool
LogTable::copy(ds_ctxt_t *ds, MYSQL *con, bool no_lock, bool finalize,
	unsigned thread_num) {
	static const size_t buf_size = 10 * 1024 * 1024;
	DBUG_ASSERT(ds);
	DBUG_ASSERT(con);
	if (m_src.empty() && !open(ds, thread_num)) {
		close();
		return false;
	}
	DBUG_ASSERT(m_src.size() == m_dst.size());

	std::unique_ptr<uchar[]> buf(new uchar[buf_size]);
	for (size_t i = 0; i < m_src.size(); ++i) {
		// .CSM can be rewritten (see write_meta_file() usage in ha_tina.cc)
		if (!finalize && ends_with(m_fnames[i].c_str(), ".CSM"))
			continue;
		size_t bytes_read;
		size_t copied_size = 0;
		while ((bytes_read = my_read(m_src[i], buf.get(), buf_size, MY_WME))) {
			if (bytes_read == size_t(-1)) {
				msg(thread_num, "error: file %s read for log table %s",
					m_fnames[i].c_str(),
					std::string("`").append(m_db).append("`.`").
					append(m_table).append("`").c_str());
				close();
				return false;
			}
			xtrabackup_io_throttling();
			if (ds_write(m_dst[i], buf.get(), bytes_read)) {
				msg(thread_num, "error: file %s write for log table %s",
					m_fnames[i].c_str(), std::string("`").append(m_db).append("`.`").
					append(m_table).append("`").c_str());
				close();
				return false;
			}
			copied_size += bytes_read;
		}
		msg(thread_num, "Copied file %s for log table %s, %zu bytes",
			m_fnames[i].c_str(), std::string("`").append(m_db).append("`.`").
					append(m_table).append("`").c_str(), copied_size);
	}

	return true;
}

class BackupImpl {
	public:
		BackupImpl(
				const char *datadir_path, ds_ctxt_t *datasink,
				std::vector<MYSQL *> &con_pool, ThreadPool &thread_pool) :
			m_datadir_path(datadir_path), m_ds(datasink), m_con_pool(con_pool),
			m_process_table_jobs(thread_pool) {}
		~BackupImpl() { }
		bool scan(
				const std::unordered_set<std::string> &exclude_tables,
				std::unordered_set<std::string> *out_processed_tables,
				bool no_lock, bool collect_log_and_stats);
		void set_post_copy_table_hook(const post_copy_table_hook_t &hook) {
			m_table_post_copy_hook = hook;
		}
		bool copy_log_tables(bool finalize);
		bool copy_stats_tables();
		bool copy_engine_binlogs(const char *binlog_dir, lsn_t backup_lsn);
		bool wait_for_finish();
		bool close_log_tables();
	private:

		void process_table_job(Table *table, bool no_lock, bool delete_table,
			bool finalize, unsigned thread_num);
		void process_binlog_job(std::string src, std::string dst,
					lsn_t backup_lsn, unsigned thread_num);

		const char *m_datadir_path;
		ds_ctxt_t *m_ds;
		std::vector<MYSQL *> &m_con_pool;
		TasksGroup m_process_table_jobs;
		std::unique_ptr<byte []> m_page_buf;

		post_copy_table_hook_t m_table_post_copy_hook;
		std::unordered_map<table_key_t, std::unique_ptr<LogTable>> m_log_tables;
		std::unordered_map<table_key_t, std::unique_ptr<Table>> m_stats_tables;
};

void BackupImpl::process_table_job(Table *table, bool no_lock,
	bool delete_table, bool finalize, unsigned thread_num) {
	int result = 0;

	if (!m_process_table_jobs.get_result())
		goto exit;

	if (!table->copy(m_ds, m_con_pool[thread_num], no_lock, finalize, thread_num))
		goto exit;

	if (m_table_post_copy_hook)
		m_table_post_copy_hook(table->get_db(), table->get_table(),
		table->get_version());

	result = 1;

exit:
	if (delete_table)
		delete table;
	m_process_table_jobs.finish_task(result);
}

void BackupImpl::process_binlog_job(std::string src, std::string dst,
				    lsn_t backup_lsn, unsigned thread_num) {
	int result = 0;
	const char *c_src= src.c_str();
	bool is_empty= true;
	lsn_t start_lsn;
	int binlog_found;

	if (!m_process_table_jobs.get_result())
		goto exit;

	binlog_found= get_binlog_header(c_src, m_page_buf.get(), start_lsn, is_empty);
	if (binlog_found > 0 && !is_empty && start_lsn <= backup_lsn) {
		// Test binlog_in_engine.mariabackup_binlogs will try to inject
		// RESET MASTER and PURGE BINARY LOGS here.
		DBUG_EXECUTE_IF("binlog_copy_sleep_2",
				if (src.find("binlog-000002.ibb") !=
				    std::string::npos)
					my_sleep(2000000););
		if (!m_ds->copy_file(c_src, dst.c_str(), thread_num))
			goto exit;
	}

	result = 1;

exit:
	m_process_table_jobs.finish_task(result);
}

bool BackupImpl::scan(const std::unordered_set<table_key_t> &exclude_tables,
	std::unordered_set<table_key_t> *out_processed_tables, bool no_lock,
	bool collect_log_and_stats) {

	msg("Start scanning common engine tables, need backup locks: %d, "
		"collect log and stat tables: %d", no_lock, collect_log_and_stats);

	std::unordered_map<table_key_t, std::unique_ptr<Table>> found_tables;

	foreach_file_in_db_dirs(m_datadir_path,
		[&](const char *file_path)->bool {

		static const char *ext_list[] =
			{".MYD", ".MYI", ".MRG", ".ARM", ".ARZ", ".CSM", ".CSV", NULL};

		bool is_aria = ends_with(file_path, ".MAD") || ends_with(file_path, ".MAI");

		if (!collect_log_and_stats && is_aria)
			return true;

		if (!is_aria && !filename_matches(file_path, ext_list))
			return true;

		if (check_if_skip_table(file_path)) {
			msg("Skipping %s.", file_path);
			return true;
		}

		auto db_table_fs = convert_filepath_to_tablename(file_path);
		auto tk =
			table_key(std::get<0>(db_table_fs), std::get<1>(db_table_fs));

		// log and stats tables are only collected in this function,
		// so there is no need to filter out them with exclude_tables.
		if (collect_log_and_stats) {
			if (is_log_table(std::get<0>(db_table_fs).c_str(),
				std::get<1>(db_table_fs).c_str())) {
				auto table_it = m_log_tables.find(tk);
				if (table_it == m_log_tables.end()) {
					msg("Log table found: %s", tk.c_str());
					table_it = m_log_tables.emplace(tk,
						std::unique_ptr<LogTable>(new LogTable(std::get<0>(db_table_fs),
						std::get<1>(db_table_fs), std::get<2>(db_table_fs)))).first;
				}
				msg("Collect log table file: %s", file_path);
				table_it->second->add_file_name(file_path);
				return true;
			}
			// Aria can handle statistics tables
			else if (is_stats_table(std::get<0>(db_table_fs).c_str(),
				std::get<1>(db_table_fs).c_str()) && !is_aria) {
				auto table_it = m_stats_tables.find(tk);
				if (table_it == m_stats_tables.end()) {
					msg("Stats table found: %s", tk.c_str());
					table_it = m_stats_tables.emplace(tk,
						std::unique_ptr<Table>(new Table(std::get<0>(db_table_fs),
						std::get<1>(db_table_fs), std::get<2>(db_table_fs)))).first;
				}
				msg("Collect stats table file: %s", file_path);
				table_it->second->add_file_name(file_path);
				return true;
			}
		} else if (is_log_table(std::get<0>(db_table_fs).c_str(),
			std::get<1>(db_table_fs).c_str()) ||
			is_stats_table(std::get<0>(db_table_fs).c_str(),
			std::get<1>(db_table_fs).c_str()))
			return true;

		if (is_aria)
			return true;

		if (exclude_tables.count(tk)) {
			msg("Skip table %s at it is in exclude list", tk.c_str());
			return true;
		}

		auto table_it = found_tables.find(tk);
		if (table_it == found_tables.end()) {
			table_it = found_tables.emplace(tk,
				std::unique_ptr<Table>(new Table(std::get<0>(db_table_fs),
				std::get<1>(db_table_fs), std::get<2>(db_table_fs)))).first;
		}

		table_it->second->add_file_name(file_path);

		return true;
	});

	for (auto &table_it : found_tables) {
		m_process_table_jobs.push_task(
			std::bind(&BackupImpl::process_table_job, this, table_it.second.release(),
					no_lock, true, false, std::placeholders::_1));
		if (out_processed_tables)
			out_processed_tables->insert(table_it.first);
	}

	msg("Stop scanning common engine tables");
	return true;
}

bool BackupImpl::copy_log_tables(bool finalize) {
	for (auto &table_it : m_log_tables) {
		// Do not execute BACKUP LOCK for log tables as it's supposed
		// that they must be copied on BLOCK_DDL and BLOCK_COMMIT locks.
		m_process_table_jobs.push_task(
			std::bind(&BackupImpl::process_table_job, this, table_it.second.get(),
					true, false, finalize, std::placeholders::_1));
	}
	return true;
}

bool BackupImpl::copy_stats_tables() {
	for (auto &table_it : m_stats_tables) {
		// Do not execute BACKUP LOCK for stats tables as it's supposed
		// that they must be copied on BLOCK_DDL and BLOCK_COMMIT locks.
		// Delete stats table object after copy (see process_table_job())
		m_process_table_jobs.push_task(
			std::bind(&BackupImpl::process_table_job, this, table_it.second.release(),
					true, true, false, std::placeholders::_1));
	}
	m_stats_tables.clear();
	return true;
}

bool BackupImpl::copy_engine_binlogs(const char *binlog_dir, lsn_t backup_lsn) {
	std::vector<std::string>files;
	std::string dir(binlog_dir && binlog_dir[0] ? binlog_dir : m_datadir_path);
	foreach_file_in_datadir(dir.c_str(),
				[&](const char *name)->bool {
					uint64_t file_no;
					if (is_binlog_name(name, &file_no))
						files.emplace_back(name);
					return true;
				});
	m_page_buf.reset(new byte [ibb_page_size]);
	for (auto &file : files) {
		std::string path(dir + "/" + file);
		m_process_table_jobs.push_task(
			std::bind(&BackupImpl::process_binlog_job, this, path,
				  file, backup_lsn, std::placeholders::_1));
	}
	return true;
}

bool BackupImpl::wait_for_finish() {
	/* Wait for threads to exit */
	return m_process_table_jobs.wait_for_finish();
}

bool BackupImpl::close_log_tables() {
	bool result = wait_for_finish();
	for (auto &table_it : m_log_tables)
		table_it.second->close();
	return result;
}

Backup::Backup(const char *datadir_path, ds_ctxt_t *datasink,
	std::vector<MYSQL *> &con_pool, ThreadPool &thread_pool) :
		m_backup_impl(
			new BackupImpl(datadir_path, datasink, con_pool,
			thread_pool)) { }

Backup::~Backup() {
	delete m_backup_impl;
}

bool Backup::scan(
	const std::unordered_set<table_key_t> &exclude_tables,
	std::unordered_set<table_key_t> *out_processed_tables,
	bool no_lock, bool collect_log_and_stats) {
	return m_backup_impl->scan(exclude_tables, out_processed_tables, no_lock,
		collect_log_and_stats);
}

bool Backup::copy_log_tables(bool finalize) {
	return m_backup_impl->copy_log_tables(finalize);
}

bool Backup::copy_stats_tables() {
	return m_backup_impl->copy_stats_tables();
}

bool Backup::copy_engine_binlogs(const char *binlog_dir, lsn_t backup_lsn) {
	return m_backup_impl->copy_engine_binlogs(binlog_dir, backup_lsn);
}

bool Backup::wait_for_finish() {
	return m_backup_impl->wait_for_finish();
}

bool Backup::close_log_tables() {
	return m_backup_impl->close_log_tables();
}

void Backup::set_post_copy_table_hook(const post_copy_table_hook_t &hook) {
	m_backup_impl->set_post_copy_table_hook(hook);
}

} // namespace common_engine
