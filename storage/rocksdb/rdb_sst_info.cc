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

/* For PRIu64 use below: */
#define __STDC_FORMAT_MACROS

#include <my_config.h>

/* This C++ file's header file */
#include "./rdb_sst_info.h"

#include <inttypes.h>

/* C++ standard header files */
#include <cstdio>
#include <string>
#include <vector>

/* MySQL header files */
#include <mysqld_error.h>
#include "../sql/log.h"
#include "./my_dir.h"

/* RocksDB header files */
#include "rocksdb/db.h"
#include "rocksdb/options.h"

/* MyRocks header files */
#include "./ha_rocksdb.h"
#include "./ha_rocksdb_proto.h"
#include "./rdb_cf_options.h"

namespace myrocks {

Rdb_sst_file::Rdb_sst_file(rocksdb::DB *const db,
                           rocksdb::ColumnFamilyHandle *const cf,
                           const rocksdb::DBOptions &db_options,
                           const std::string &name, const bool tracing)
    : m_db(db), m_cf(cf), m_db_options(db_options), m_sst_file_writer(nullptr),
      m_name(name), m_tracing(tracing) {
  DBUG_ASSERT(db != nullptr);
  DBUG_ASSERT(cf != nullptr);
}

Rdb_sst_file::~Rdb_sst_file() {
  // Make sure we clean up
  delete m_sst_file_writer;
  m_sst_file_writer = nullptr;

  // In case something went wrong attempt to delete the temporary file.
  // If everything went fine that file will have been renamed and this
  // function call will fail.
  std::remove(m_name.c_str());
}

rocksdb::Status Rdb_sst_file::open() {
  DBUG_ASSERT(m_sst_file_writer == nullptr);

  rocksdb::ColumnFamilyDescriptor cf_descr;

  rocksdb::Status s = m_cf->GetDescriptor(&cf_descr);
  if (!s.ok()) {
    return s;
  }

  // Create an sst file writer with the current options and comparator
  const rocksdb::Comparator *comparator = m_cf->GetComparator();

  const rocksdb::EnvOptions env_options(m_db_options);
  const rocksdb::Options options(m_db_options, cf_descr.options);

  m_sst_file_writer =
      new rocksdb::SstFileWriter(env_options, options, comparator, m_cf);

  s = m_sst_file_writer->Open(m_name);
  if (m_tracing) {
    // NO_LINT_DEBUG
    sql_print_information("SST Tracing: Open(%s) returned %s", m_name.c_str(),
                          s.ok() ? "ok" : "not ok");
  }

  if (!s.ok()) {
    delete m_sst_file_writer;
    m_sst_file_writer = nullptr;
  }

  return s;
}

rocksdb::Status Rdb_sst_file::put(const rocksdb::Slice &key,
                                  const rocksdb::Slice &value) {
  DBUG_ASSERT(m_sst_file_writer != nullptr);

  // Add the specified key/value to the sst file writer
  return m_sst_file_writer->Add(key, value);
}

std::string Rdb_sst_file::generateKey(const std::string &key) {
  static char const hexdigit[] = {'0', '1', '2', '3', '4', '5', '6', '7',
                                  '8', '9', 'A', 'B', 'C', 'D', 'E', 'F'};

  std::string res;

  res.reserve(key.size() * 2);

  for (auto ch : key) {
    res += hexdigit[((uint8_t)ch) >> 4];
    res += hexdigit[((uint8_t)ch) & 0x0F];
  }

  return res;
}

// This function is run by the background thread
rocksdb::Status Rdb_sst_file::commit() {
  DBUG_ASSERT(m_sst_file_writer != nullptr);

  rocksdb::Status s;
  rocksdb::ExternalSstFileInfo fileinfo; /// Finish may should be modified

  // Close out the sst file
  s = m_sst_file_writer->Finish(&fileinfo);
  if (m_tracing) {
    // NO_LINT_DEBUG
    sql_print_information("SST Tracing: Finish returned %s",
                          s.ok() ? "ok" : "not ok");
  }

  if (s.ok()) {
    if (m_tracing) {
      // NO_LINT_DEBUG
      sql_print_information("SST Tracing: Adding file %s, smallest key: %s, "
                            "largest key: %s, file size: %" PRIu64 ", "
                            "num_entries: %" PRIu64,
                            fileinfo.file_path.c_str(),
                            generateKey(fileinfo.smallest_key).c_str(),
                            generateKey(fileinfo.largest_key).c_str(),
                            fileinfo.file_size, fileinfo.num_entries);
    }

    // Add the file to the database
    // Set the snapshot_consistency parameter to false since no one
    // should be accessing the table we are bulk loading
    rocksdb::IngestExternalFileOptions opts;
    opts.move_files = true;
    opts.snapshot_consistency = false;
    opts.allow_global_seqno = false;
    opts.allow_blocking_flush = false;
    s = m_db->IngestExternalFile(m_cf, {m_name}, opts);

    if (m_tracing) {
      // NO_LINT_DEBUG
      sql_print_information("SST Tracing: AddFile(%s) returned %s",
                            fileinfo.file_path.c_str(),
                            s.ok() ? "ok" : "not ok");
    }
  }

  delete m_sst_file_writer;
  m_sst_file_writer = nullptr;

  return s;
}

Rdb_sst_info::Rdb_sst_info(rocksdb::DB *const db, const std::string &tablename,
                           const std::string &indexname,
                           rocksdb::ColumnFamilyHandle *const cf,
                           const rocksdb::DBOptions &db_options,
                           const bool &tracing)
    : m_db(db), m_cf(cf), m_db_options(db_options), m_curr_size(0),
      m_sst_count(0), m_error_msg(""),
#if defined(RDB_SST_INFO_USE_THREAD)
      m_queue(), m_mutex(), m_cond(), m_thread(nullptr), m_finished(false),
#endif
      m_sst_file(nullptr), m_tracing(tracing) {
  m_prefix = db->GetName() + "/";

  std::string normalized_table;
  if (rdb_normalize_tablename(tablename.c_str(), &normalized_table)) {
    // We failed to get a normalized table name.  This should never happen,
    // but handle it anyway.
    m_prefix += "fallback_" + std::to_string(reinterpret_cast<intptr_t>(
                                  reinterpret_cast<void *>(this))) +
                "_" + indexname + "_";
  } else {
    m_prefix += normalized_table + "_" + indexname + "_";
  }

  // Unique filename generated to prevent collisions when the same table
  // is loaded in parallel
  m_prefix += std::to_string(m_prefix_counter.fetch_add(1)) + "_";

  rocksdb::ColumnFamilyDescriptor cf_descr;
  const rocksdb::Status s = m_cf->GetDescriptor(&cf_descr);
  if (!s.ok()) {
    // Default size if we can't get the cf's target size
    m_max_size = 64 * 1024 * 1024;
  } else {
    // Set the maximum size to 3 times the cf's target size
    m_max_size = cf_descr.options.target_file_size_base * 3;
  }
}

Rdb_sst_info::~Rdb_sst_info() {
  DBUG_ASSERT(m_sst_file == nullptr);
#if defined(RDB_SST_INFO_USE_THREAD)
  DBUG_ASSERT(m_thread == nullptr);
#endif
}

int Rdb_sst_info::open_new_sst_file() {
  DBUG_ASSERT(m_sst_file == nullptr);

  // Create the new sst file's name
  const std::string name = m_prefix + std::to_string(m_sst_count++) + m_suffix;

  // Create the new sst file object
  m_sst_file = new Rdb_sst_file(m_db, m_cf, m_db_options, name, m_tracing);

  // Open the sst file
  const rocksdb::Status s = m_sst_file->open();
  if (!s.ok()) {
    set_error_msg(m_sst_file->get_name(), s.ToString());
    delete m_sst_file;
    m_sst_file = nullptr;
    return HA_EXIT_FAILURE;
  }

  m_curr_size = 0;

  return HA_EXIT_SUCCESS;
}

void Rdb_sst_info::close_curr_sst_file() {
  DBUG_ASSERT(m_sst_file != nullptr);
  DBUG_ASSERT(m_curr_size > 0);

#if defined(RDB_SST_INFO_USE_THREAD)
  if (m_thread == nullptr) {
    // We haven't already started a background thread, so start one
    m_thread = new std::thread(thread_fcn, this);
  }

  DBUG_ASSERT(m_thread != nullptr);

  {
    // Add this finished sst file to the queue (while holding mutex)
    const std::lock_guard<std::mutex> guard(m_mutex);
    m_queue.push(m_sst_file);
  }

  // Notify the background thread that there is a new entry in the queue
  m_cond.notify_one();
#else
  const rocksdb::Status s = m_sst_file->commit();
  if (!s.ok()) {
    set_error_msg(m_sst_file->get_name(), s.ToString());
  }

  delete m_sst_file;
#endif

  // Reset for next sst file
  m_sst_file = nullptr;
  m_curr_size = 0;
}

int Rdb_sst_info::put(const rocksdb::Slice &key, const rocksdb::Slice &value) {
  int rc;

  if (m_curr_size >= m_max_size) {
    // The current sst file has reached its maximum, close it out
    close_curr_sst_file();

    // While we are here, check to see if we have had any errors from the
    // background thread - we don't want to wait for the end to report them
    if (!m_error_msg.empty()) {
      return HA_EXIT_FAILURE;
    }
  }

  if (m_curr_size == 0) {
    // We don't have an sst file open - open one
    rc = open_new_sst_file();
    if (rc != 0) {
      return rc;
    }
  }

  DBUG_ASSERT(m_sst_file != nullptr);

  // Add the key/value to the current sst file
  const rocksdb::Status s = m_sst_file->put(key, value);
  if (!s.ok()) {
    set_error_msg(m_sst_file->get_name(), s.ToString());
    return HA_EXIT_FAILURE;
  }

  m_curr_size += key.size() + value.size();

  return HA_EXIT_SUCCESS;
}

int Rdb_sst_info::commit() {
  if (m_curr_size > 0) {
    // Close out any existing files
    close_curr_sst_file();
  }

#if defined(RDB_SST_INFO_USE_THREAD)
  if (m_thread != nullptr) {
    // Tell the background thread we are done
    m_finished = true;
    m_cond.notify_one();

    // Wait for the background thread to finish
    m_thread->join();
    delete m_thread;
    m_thread = nullptr;
  }
#endif

  // Did we get any errors?
  if (!m_error_msg.empty()) {
    return HA_EXIT_FAILURE;
  }

  return HA_EXIT_SUCCESS;
}

void Rdb_sst_info::set_error_msg(const std::string &sst_file_name,
                                 const std::string &msg) {
#if defined(RDB_SST_INFO_USE_THREAD)
  // Both the foreground and background threads can set the error message
  // so lock the mutex to protect it.  We only want the first error that
  // we encounter.
  const std::lock_guard<std::mutex> guard(m_mutex);
#endif
  my_printf_error(ER_UNKNOWN_ERROR, "[%s] bulk load error: %s", MYF(0),
                  sst_file_name.c_str(), msg.c_str());
  if (m_error_msg.empty()) {
    m_error_msg = "[" + sst_file_name + "] " + msg;
  }
}

#if defined(RDB_SST_INFO_USE_THREAD)
// Static thread function - the Rdb_sst_info object is in 'object'
void Rdb_sst_info::thread_fcn(void *object) {
  reinterpret_cast<Rdb_sst_info *>(object)->run_thread();
}

void Rdb_sst_info::run_thread() {
  const std::unique_lock<std::mutex> lk(m_mutex);

  do {
    // Wait for notification or 1 second to pass
    m_cond.wait_for(lk, std::chrono::seconds(1));

    // Inner loop pulls off all Rdb_sst_file entries and processes them
    while (!m_queue.empty()) {
      const Rdb_sst_file *const sst_file = m_queue.front();
      m_queue.pop();

      // Release the lock - we don't want to hold it while committing the file
      lk.unlock();

      // Close out the sst file and add it to the database
      const rocksdb::Status s = sst_file->commit();
      if (!s.ok()) {
        set_error_msg(sst_file->get_name(), s.ToString());
      }

      delete sst_file;

      // Reacquire the lock for the next inner loop iteration
      lk.lock();
    }

    // If the queue is empty and the main thread has indicated we should exit
    // break out of the loop.
  } while (!m_finished);

  DBUG_ASSERT(m_queue.empty());
}
#endif

void Rdb_sst_info::init(const rocksdb::DB *const db) {
  const std::string path = db->GetName() + FN_DIRSEP;
  struct st_my_dir *const dir_info = my_dir(path.c_str(), MYF(MY_DONT_SORT));

  // Access the directory
  if (dir_info == nullptr) {
    // NO_LINT_DEBUG
    sql_print_warning("RocksDB: Could not access database directory: %s",
                      path.c_str());
    return;
  }

  // Scan through the files in the directory
  const struct fileinfo *file_info = dir_info->dir_entry;
  for (uint ii= 0; ii < dir_info->number_of_files; ii++, file_info++) {
    // find any files ending with m_suffix ...
    const std::string name = file_info->name;
    const size_t pos = name.find(m_suffix);
    if (pos != std::string::npos && name.size() - pos == m_suffix.size()) {
      // ... and remove them
      const std::string fullname = path + name;
      my_delete(fullname.c_str(), MYF(0));
    }
  }

  // Release the directory entry
  my_dirend(dir_info);
}

std::atomic<uint64_t> Rdb_sst_info::m_prefix_counter(0);
std::string Rdb_sst_info::m_suffix = ".bulk_load.tmp";
} // namespace myrocks
