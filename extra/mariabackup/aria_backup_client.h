#pragma once
#include "my_global.h"
#include "datasink.h"
#include "backup_mysql.h"
#include "thread_pool.h"
#include "xtrabackup.h"

namespace aria {

bool prepare(const char *target_dir);

class BackupImpl;

class Backup {
	public:
		Backup(const char *datadir_path,
		       const char *aria_log_path,
		       ds_ctxt_t *datasink,
			std::vector<MYSQL *> &con_pool, ThreadPool &thread_pool);
		~Backup();
		Backup (Backup &&other) = delete;
		Backup & operator= (Backup &&other) = delete;
		Backup(const Backup &) = delete;
		Backup & operator= (const Backup &) = delete;
		bool init();
		bool start(bool no_lock);
		bool wait_for_finish();
		bool copy_offline_tables(
			const std::unordered_set<table_key_t> *exclude_tables, bool no_lock,
			bool copy_stats);
		bool finalize();
		bool copy_log_tail();
		void set_post_copy_table_hook(const post_copy_table_hook_t &hook);
	private:
		BackupImpl *m_backup_impl;
};

} // namespace aria
