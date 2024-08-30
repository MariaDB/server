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
   Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02111-1301 USA */

/* For PRIu64 use below: */
#define __STDC_FORMAT_MACROS

#include <my_global.h>

/* This C++ file's header file */
#include "./rdb_sst_info.h"

#include <inttypes.h>

/* C++ standard header files */
#include <cstdio>
#include <string>
#include <utility>
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
#include "./rdb_psi.h"

namespace myrocks {

Rdb_sst_file_ordered::Rdb_sst_file::Rdb_sst_file(
    rocksdb::DB *const db, rocksdb::ColumnFamilyHandle *const cf,
    const rocksdb::DBOptions &db_options, const std::string &name,
    const bool tracing)
    : m_db(db),
      m_cf(cf),
      m_db_options(db_options),
      m_sst_file_writer(nullptr),
      m_name(name),
      m_tracing(tracing),
      m_comparator(cf->GetComparator()) {
  DBUG_ASSERT(db != nullptr);
  DBUG_ASSERT(cf != nullptr);
}

Rdb_sst_file_ordered::Rdb_sst_file::~Rdb_sst_file() {
  // Make sure we clean up
  delete m_sst_file_writer;
  m_sst_file_writer = nullptr;
}

rocksdb::Status Rdb_sst_file_ordered::Rdb_sst_file::open() {
  DBUG_ASSERT(m_sst_file_writer == nullptr);

  rocksdb::ColumnFamilyDescriptor cf_descr;

  rocksdb::Status s = m_cf->GetDescriptor(&cf_descr);
  if (!s.ok()) {
    return s;
  }

  // Create an sst file writer with the current options and comparator
  const rocksdb::EnvOptions env_options(m_db_options);
  const rocksdb::Options options(m_db_options, cf_descr.options);

  m_sst_file_writer =
      new rocksdb::SstFileWriter(env_options, options, m_comparator, m_cf, true,
                                 rocksdb::Env::IOPriority::IO_TOTAL,
                                 cf_descr.options.optimize_filters_for_hits);

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

rocksdb::Status Rdb_sst_file_ordered::Rdb_sst_file::put(
    const rocksdb::Slice &key, const rocksdb::Slice &value) {
  DBUG_ASSERT(m_sst_file_writer != nullptr);

#ifdef __GNUC__
  // Add the specified key/value to the sst file writer
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#endif
#ifdef _MSC_VER
#pragma warning (disable : 4996)
#endif
  return m_sst_file_writer->Add(key, value);
}

std::string Rdb_sst_file_ordered::Rdb_sst_file::generateKey(
    const std::string &key) {
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
rocksdb::Status Rdb_sst_file_ordered::Rdb_sst_file::commit() {
  DBUG_ASSERT(m_sst_file_writer != nullptr);

  rocksdb::Status s;
  rocksdb::ExternalSstFileInfo fileinfo;  /// Finish may should be modified

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
      sql_print_information(
          "SST Tracing: Adding file %s, smallest key: %s, "
          "largest key: %s, file size: %" PRIu64
          ", "
          "num_entries: %" PRIu64,
          fileinfo.file_path.c_str(),
          generateKey(fileinfo.smallest_key).c_str(),
          generateKey(fileinfo.largest_key).c_str(), fileinfo.file_size,
          fileinfo.num_entries);
    }
  }

  delete m_sst_file_writer;
  m_sst_file_writer = nullptr;

  return s;
}

void Rdb_sst_file_ordered::Rdb_sst_stack::push(const rocksdb::Slice &key,
                                               const rocksdb::Slice &value) {
  if (m_buffer == nullptr) {
    m_buffer = new char[m_buffer_size];
  }

  // Put the actual key and value data unto our stack
  size_t key_offset = m_offset;
  memcpy(m_buffer + m_offset, key.data(), key.size());
  m_offset += key.size();
  memcpy(m_buffer + m_offset, value.data(), value.size());
  m_offset += value.size();

  // Push just the offset, the key length and the value length onto the stack
  m_stack.push(std::make_tuple(key_offset, key.size(), value.size()));
}

std::pair<rocksdb::Slice, rocksdb::Slice>
Rdb_sst_file_ordered::Rdb_sst_stack::top() {
  size_t offset, key_len, value_len;
  // Pop the next item off the internal stack
  std::tie(offset, key_len, value_len) = m_stack.top();

  // Make slices from the offset (first), key length (second), and value
  // length (third)
  DBUG_ASSERT(m_buffer != nullptr);
  rocksdb::Slice key(m_buffer + offset, key_len);
  rocksdb::Slice value(m_buffer + offset + key_len, value_len);

  return std::make_pair(key, value);
}

Rdb_sst_file_ordered::Rdb_sst_file_ordered(
    rocksdb::DB *const db, rocksdb::ColumnFamilyHandle *const cf,
    const rocksdb::DBOptions &db_options, const std::string &name,
    const bool tracing, size_t max_size)
    : m_use_stack(false),
      m_first(true),
      m_stack(max_size),
      m_file(db, cf, db_options, name, tracing) {
  m_stack.reset();
}

rocksdb::Status Rdb_sst_file_ordered::apply_first() {
  rocksdb::Slice first_key_slice(m_first_key);
  rocksdb::Slice first_value_slice(m_first_value);
  rocksdb::Status s;

  if (m_use_stack) {
    // Put the first key onto the stack
    m_stack.push(first_key_slice, first_value_slice);
  } else {
    // Put the first key into the SST
    s = m_file.put(first_key_slice, first_value_slice);
    if (!s.ok()) {
      return s;
    }
  }

  // Clear out the 'first' strings for next key/value
  m_first_key.clear();
  m_first_value.clear();

  return s;
}

rocksdb::Status Rdb_sst_file_ordered::put(const rocksdb::Slice &key,
                                          const rocksdb::Slice &value) {
  rocksdb::Status s;

  // If this is the first key, just store a copy of the key and value
  if (m_first) {
    m_first_key = key.ToString();
    m_first_value = value.ToString();
    m_first = false;
    return rocksdb::Status::OK();
  }

  // If the first key is not empty we must be the second key.  Compare the
  // new key with the first key to determine if the data will go straight
  // the SST or be put on the stack to be retrieved later.
  if (!m_first_key.empty()) {
    rocksdb::Slice first_key_slice(m_first_key);
    int cmp = m_file.compare(first_key_slice, key);
    m_use_stack = (cmp > 0);

    // Apply the first key to the stack or SST
    s = apply_first();
    if (!s.ok()) {
      return s;
    }
  }

  // Put this key on the stack or into the SST
  if (m_use_stack) {
    m_stack.push(key, value);
  } else {
    s = m_file.put(key, value);
  }

  return s;
}

rocksdb::Status Rdb_sst_file_ordered::commit() {
  rocksdb::Status s;

  // Make sure we get the first key if it was the only key given to us.
  if (!m_first_key.empty()) {
    s = apply_first();
    if (!s.ok()) {
      return s;
    }
  }

  if (m_use_stack) {
    rocksdb::Slice key;
    rocksdb::Slice value;

    // We are ready to commit, pull each entry off the stack (which reverses
    // the original data) and send it to the SST file.
    while (!m_stack.empty()) {
      std::tie(key, value) = m_stack.top();
      s = m_file.put(key, value);
      if (!s.ok()) {
        return s;
      }

      m_stack.pop();
    }

    // We have pulled everything off the stack, reset for the next time
    m_stack.reset();
    m_use_stack = false;
  }

  // reset m_first
  m_first = true;

  return m_file.commit();
}

Rdb_sst_info::Rdb_sst_info(rocksdb::DB *const db, const std::string &tablename,
                           const std::string &indexname,
                           rocksdb::ColumnFamilyHandle *const cf,
                           const rocksdb::DBOptions &db_options,
                           const bool tracing)
    : m_db(db),
      m_cf(cf),
      m_db_options(db_options),
      m_curr_size(0),
      m_sst_count(0),
      m_background_error(HA_EXIT_SUCCESS),
      m_done(false),
      m_sst_file(nullptr),
      m_tracing(tracing),
      m_print_client_error(true) {
  m_prefix = db->GetName() + "/";

  std::string normalized_table;
  if (rdb_normalize_tablename(tablename.c_str(), &normalized_table)) {
    // We failed to get a normalized table name.  This should never happen,
    // but handle it anyway.
    m_prefix += "fallback_" +
                std::to_string(reinterpret_cast<intptr_t>(
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
  mysql_mutex_init(rdb_sst_commit_key, &m_commit_mutex, MY_MUTEX_INIT_FAST);
}

Rdb_sst_info::~Rdb_sst_info() {
  DBUG_ASSERT(m_sst_file == nullptr);

  for (auto sst_file : m_committed_files) {
    // In case something went wrong attempt to delete the temporary file.
    // If everything went fine that file will have been renamed and this
    // function call will fail.
    std::remove(sst_file.c_str());
  }
  m_committed_files.clear();

  mysql_mutex_destroy(&m_commit_mutex);
}

int Rdb_sst_info::open_new_sst_file() {
  DBUG_ASSERT(m_sst_file == nullptr);

  // Create the new sst file's name
  const std::string name = m_prefix + std::to_string(m_sst_count++) + m_suffix;

  // Create the new sst file object
  m_sst_file = new Rdb_sst_file_ordered(m_db, m_cf, m_db_options, name,
                                        m_tracing, m_max_size);

  // Open the sst file
  const rocksdb::Status s = m_sst_file->open();
  if (!s.ok()) {
    set_error_msg(m_sst_file->get_name(), s);
    delete m_sst_file;
    m_sst_file = nullptr;
    return HA_ERR_ROCKSDB_BULK_LOAD;
  }

  m_curr_size = 0;

  return HA_EXIT_SUCCESS;
}

void Rdb_sst_info::commit_sst_file(Rdb_sst_file_ordered *sst_file) {
  const rocksdb::Status s = sst_file->commit();
  if (!s.ok()) {
    set_error_msg(sst_file->get_name(), s);
    set_background_error(HA_ERR_ROCKSDB_BULK_LOAD);
  }

  m_committed_files.push_back(sst_file->get_name());

  delete sst_file;
}

void Rdb_sst_info::close_curr_sst_file() {
  DBUG_ASSERT(m_sst_file != nullptr);
  DBUG_ASSERT(m_curr_size > 0);

  commit_sst_file(m_sst_file);

  // Reset for next sst file
  m_sst_file = nullptr;
  m_curr_size = 0;
}

int Rdb_sst_info::put(const rocksdb::Slice &key, const rocksdb::Slice &value) {
  int rc;

  DBUG_ASSERT(!m_done);

  if (m_curr_size + key.size() + value.size() >= m_max_size) {
    // The current sst file has reached its maximum, close it out
    close_curr_sst_file();

    // While we are here, check to see if we have had any errors from the
    // background thread - we don't want to wait for the end to report them
    if (have_background_error()) {
      return get_and_reset_background_error();
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
    set_error_msg(m_sst_file->get_name(), s);
    return HA_ERR_ROCKSDB_BULK_LOAD;
  }

  m_curr_size += key.size() + value.size();

  return HA_EXIT_SUCCESS;
}

/*
  Finish the current work and return the list of SST files ready to be
  ingested. This function need to be idempotent and atomic
 */
int Rdb_sst_info::finish(Rdb_sst_commit_info *commit_info,
                         bool print_client_error) {
  int ret = HA_EXIT_SUCCESS;

  // Both the transaction clean up and the ha_rocksdb handler have
  // references to this Rdb_sst_info and both can call commit, so
  // synchronize on the object here.
  // This also means in such case the bulk loading operation stop being truly
  // atomic, and we should consider fixing this in the future
  RDB_MUTEX_LOCK_CHECK(m_commit_mutex);

  if (is_done()) {
    RDB_MUTEX_UNLOCK_CHECK(m_commit_mutex);
    return ret;
  }

  m_print_client_error = print_client_error;

  if (m_curr_size > 0) {
    // Close out any existing files
    close_curr_sst_file();
  }

  // This checks out the list of files so that the caller can collect/group
  // them and ingest them all in one go, and any racing calls to commit
  // won't see them at all
  commit_info->init(m_cf, std::move(m_committed_files));
  DBUG_ASSERT(m_committed_files.size() == 0);

  m_done = true;
  RDB_MUTEX_UNLOCK_CHECK(m_commit_mutex);

  // Did we get any errors?
  if (have_background_error()) {
    ret = get_and_reset_background_error();
  }

  m_print_client_error = true;
  return ret;
}

void Rdb_sst_info::set_error_msg(const std::string &sst_file_name,
                                 const rocksdb::Status &s) {
  if (!m_print_client_error) return;

  report_error_msg(s, sst_file_name.c_str());
}

void Rdb_sst_info::report_error_msg(const rocksdb::Status &s,
                                    const char *sst_file_name) {
  if (s.IsInvalidArgument() &&
      strcmp(s.getState(), "Keys must be added in strict ascending order.") == 0) {
    my_printf_error(ER_KEYS_OUT_OF_ORDER,
                    "Rows must be inserted in primary key order "
                    "during bulk load operation",
                    MYF(0));
  } else if (s.IsInvalidArgument() &&
             strcmp(s.getState(), "Global seqno is required, but disabled") ==
                 0) {
    my_printf_error(ER_OVERLAPPING_KEYS,
                    "Rows inserted during bulk load "
                    "must not overlap existing rows",
                    MYF(0));
  } else {
    my_printf_error(ER_UNKNOWN_ERROR, "[%s] bulk load error: %s", MYF(0),
                    sst_file_name, s.ToString().c_str());
  }
}

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
  for (size_t ii= 0; ii < dir_info->number_of_files; ii++, file_info++) {
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
}  // namespace myrocks
