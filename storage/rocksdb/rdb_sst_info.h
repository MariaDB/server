/*
   Copyright (c) 2016, Facebook, Inc.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA */

#pragma once

/* C++ standard header files */
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <vector>

/* RocksDB header files */
#include "rocksdb/db.h"
#include "rocksdb/sst_file_writer.h"

// define RDB_SST_INFO_USE_THREAD /* uncomment to use threads */

namespace myrocks {

class Rdb_sst_file {
private:
  Rdb_sst_file(const Rdb_sst_file &p) = delete;
  Rdb_sst_file &operator=(const Rdb_sst_file &p) = delete;

  rocksdb::DB *const m_db;
  rocksdb::ColumnFamilyHandle *const m_cf;
  const rocksdb::DBOptions &m_db_options;
  rocksdb::SstFileWriter *m_sst_file_writer;
  const std::string m_name;
  const bool m_tracing;

  std::string generateKey(const std::string &key);

public:
  Rdb_sst_file(rocksdb::DB *const db, rocksdb::ColumnFamilyHandle *const cf,
               const rocksdb::DBOptions &db_options, const std::string &name,
               const bool tracing);
  ~Rdb_sst_file();

  rocksdb::Status open();
  rocksdb::Status put(const rocksdb::Slice &key, const rocksdb::Slice &value);
  rocksdb::Status commit();
  const std::string get_name() const { return m_name; }
};

class Rdb_sst_info {
private:
  Rdb_sst_info(const Rdb_sst_info &p) = delete;
  Rdb_sst_info &operator=(const Rdb_sst_info &p) = delete;

  rocksdb::DB *const m_db;
  rocksdb::ColumnFamilyHandle *const m_cf;
  const rocksdb::DBOptions &m_db_options;
  uint64_t m_curr_size;
  uint64_t m_max_size;
  uint m_sst_count;
  std::string m_error_msg;
  std::string m_prefix;
  static std::atomic<uint64_t> m_prefix_counter;
  static std::string m_suffix;
#if defined(RDB_SST_INFO_USE_THREAD)
  std::queue<Rdb_sst_file *> m_queue;
  std::mutex m_mutex;
  std::condition_variable m_cond;
  std::thread *m_thread;
  bool m_finished;
#endif
  Rdb_sst_file *m_sst_file;
  const bool m_tracing;

  int open_new_sst_file();
  void close_curr_sst_file();
  void set_error_msg(const std::string &sst_file_name, const std::string &msg);

#if defined(RDB_SST_INFO_USE_THREAD)
  void run_thread();

  static void thread_fcn(void *object);
#endif

public:
  Rdb_sst_info(rocksdb::DB *const db, const std::string &tablename,
               const std::string &indexname,
               rocksdb::ColumnFamilyHandle *const cf,
               const rocksdb::DBOptions &db_options, const bool &tracing);
  ~Rdb_sst_info();

  int put(const rocksdb::Slice &key, const rocksdb::Slice &value);
  int commit();

  const std::string &error_message() const { return m_error_msg; }

  static void init(const rocksdb::DB *const db);
};

} // namespace myrocks
