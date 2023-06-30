#pragma once
#include "my_global.h"
#include "backup_mysql.h"
#include "datasink.h"
#include "thread_pool.h"
#include "xtrabackup.h"

#include <unordered_set>
#include <string>
#include <vector>

namespace common_engine {

class BackupImpl;

class Backup {
	public:
		Backup(const char *datadir_path, ds_ctxt_t *datasink,
				std::vector<MYSQL *> &con_pool, ThreadPool &thread_pool);
		~Backup();
		Backup (Backup &&other) = delete;
		Backup & operator= (Backup &&other) = delete;
		Backup(const Backup &) = delete;
		Backup & operator= (const Backup &) = delete;
		bool scan(
				const std::unordered_set<table_key_t> &exclude_tables,
				std::unordered_set<table_key_t> *out_processed_tables,
				bool no_lock, bool collect_log_and_stats);
		bool copy_log_tables(bool finalize);
		bool copy_stats_tables();
		bool wait_for_finish();
		bool close_log_tables();
		void set_post_copy_table_hook(const post_copy_table_hook_t &hook);
	private:
		BackupImpl *m_backup_impl;
};

} // namespace common_engine

