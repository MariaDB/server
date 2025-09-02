/*
   Copyright (c) 2024, 2024, MariaDB Corporation.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA
*/

/**
@file storage/maria/ma_clone.cc
Clone Aria Tables
Part of the implementation is taken from extra/mariabackup/aria_backup_client.cc
and plugin/clone/src/clone_se.cc
*/

#include "handler.h"
#include "clone_handler.h"
#include "mysqld_error.h"
#include "maria_def.h"
#include <aria_backup.h>
#include "log.h"

#include <array>
#include <condition_variable>
#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
#include <queue>
#include <unordered_set>
#include <vector>
#include <sstream>
#include <iomanip>

#ifdef EMBEDDED_LIBRARY
int clone_backup_lock(THD *thd, const char *, const char*) { return 0; }
int clone_backup_unlock(THD *thd) { return 0; }
namespace clone_common
{
  std::string read_table_version_id(File f) { return ""; }
  std::tuple<std::string, std::string, std::string>
  convert_filepath_to_tablename(const char *filepath)
  { return std::make_tuple("", "", ""); }
  
  bool is_stats_table(const char *db, const char *table)
  { return false; }

  bool is_log_table(const char *db, const char *table)
  { return false; }
}

#endif

namespace aria_engine
{
class Locator
{
 public:
  Locator(const Locator *ref_loc, uint32_t clone_index, bool is_copy);
  Locator(const unsigned char *serial, size_t serial_length);

  std::pair<const unsigned char *, uint32_t> get_locator() const;
  bool operator==(const Locator& other) const;
  uint32_t index() const { return m_index; }

  static constexpr uint32_t S_CUR_VERSION= 1;
  static constexpr size_t S_MAX_LENGTH= 12;

 private:
  void serialize();
  void deserialize();

 private:
  uint32_t m_version= S_CUR_VERSION;
  uint32_t m_clone_id= 0;
  uint32_t m_index= 0;
  unsigned char m_serial[S_MAX_LENGTH];
};

Locator::Locator(const unsigned char *serial, size_t serial_length)
{
  DBUG_ASSERT(serial_length == S_MAX_LENGTH);
  memset(&m_serial[0], 0, S_MAX_LENGTH);
  auto cp_length= std::min<size_t>(serial_length, S_MAX_LENGTH);
  memcpy(&m_serial[0], serial, cp_length);
  deserialize();
}

void Locator::serialize()
{
  unsigned char *ptr= &m_serial[0];
  int4store(ptr, m_version);
  ptr+= 4;
  int4store(ptr, m_clone_id);
  ptr+= 4;
  int4store(ptr, m_index);
}

void Locator::deserialize()
{
  unsigned char *ptr= &m_serial[0];
  m_version= uint4korr(ptr);
  ptr+= 4;
  m_clone_id= uint4korr(ptr);
  ptr+= 4;
  m_index= uint4korr(ptr);
}

std::pair<const unsigned char *, uint32_t> Locator::get_locator() const
{
  return std::make_pair(&m_serial[0], static_cast<uint32_t>(S_MAX_LENGTH));
}

bool Locator::operator==(const Locator& other) const
{
  if (m_clone_id != other.m_clone_id)
    return false;
  DBUG_ASSERT(m_version == other.m_version);
  DBUG_ASSERT(m_index == other.m_index);
  return (m_version == other.m_version && m_index == other.m_index);
}

class Descriptor
{
 public:
  Descriptor(const unsigned char *serial, size_t serial_length);
  Descriptor(const std::string &file_name, uint64_t offset, bool is_log);

  std::pair<std::string, uint64_t> get_file_info() const;
  std::pair<const unsigned char *, uint32_t> get_descriptor() const;
  bool is_log() const { return m_is_log; }

  static constexpr size_t S_MAX_META_LENGTH= 16;
  static constexpr size_t S_MAX_LENGTH= S_MAX_META_LENGTH + 2 * FN_REFLEN + 1;
  /* Special offset values. */
  static constexpr uint64_t S_OFF_APPEND= std::numeric_limits<uint64_t>::max();
  static constexpr uint64_t S_OFF_NO_DATA= S_OFF_APPEND - 1;

  const uint32_t DESC_FLAG_REDO= 0x01;

 private:
  uint64_t m_file_offset= 0;
  /* Part of 4 byte serialized flags. */
  bool m_is_log= false;
  size_t m_file_name_len= 0;
  unsigned char m_serial[S_MAX_LENGTH];
};

Descriptor::Descriptor(const unsigned char *serial, size_t serial_length)
{
  DBUG_ASSERT(serial_length <= S_MAX_LENGTH);
  memset(&m_serial[0], 0, S_MAX_LENGTH);
  auto cp_length= std::min<size_t>(serial_length, S_MAX_LENGTH);
  memcpy(&m_serial[0], serial, cp_length);

  unsigned char *ptr= &m_serial[0];
  m_file_offset= uint8korr(ptr);
  ptr+= 8;
  uint32_t flags= uint4korr(ptr);
  ptr+= 4;
  m_file_name_len= uint4korr(ptr);
  m_is_log= flags & DESC_FLAG_REDO;
}

Descriptor::Descriptor(const std::string &file_name, uint64_t offset,
                       bool is_log)
{
  m_file_offset= offset;
  m_is_log= is_log;
  m_file_name_len= file_name.length();
  unsigned char *ptr= &m_serial[0];
  memset(ptr, 0, S_MAX_LENGTH);

  int8store(ptr, offset);
  ptr+= 8;

  uint32_t flags= 0;
  if (m_is_log) flags |= DESC_FLAG_REDO;
  int4store(ptr, flags);
  ptr+= 4;

  int4store(ptr, static_cast<uint32_t>(m_file_name_len));
  ptr+= 4;

  if (m_file_name_len)
  {
    size_t available_length= S_MAX_LENGTH - S_MAX_META_LENGTH;
    uint32_t cp_length= static_cast<uint32_t>(
      std::min(m_file_name_len, available_length));
    memcpy(ptr, file_name.c_str(), cp_length);
  }
}

std::pair<std::string, uint64_t> Descriptor::get_file_info() const
{
  auto ptr= reinterpret_cast<const char *>(&m_serial[0]);
  ptr+= S_MAX_META_LENGTH;
  return std::make_pair(std::string(ptr, m_file_name_len), m_file_offset);
}

std::pair<const unsigned char *, uint32_t> Descriptor::get_descriptor() const
{
  auto length= static_cast<uint32_t>(m_file_name_len + S_MAX_META_LENGTH);
  return std::make_pair(&m_serial[0], length);
}

static int send_data(Ha_clone_cbk *cbk_ctx, const unsigned char* data,
                     size_t data_len, uint64_t offset,
                     const std::string &file_name, bool log_file= false)
{
  Descriptor data_desc(file_name, offset, log_file);
  auto [desc, desc_len]= data_desc.get_descriptor();
  cbk_ctx->set_data_desc(desc, desc_len);
  cbk_ctx->clear_flags();
  cbk_ctx->set_os_buffer_cache();
  return cbk_ctx->buffer_cbk(const_cast<unsigned char*>(data),
                             static_cast<uint>(data_len));
}

static int send_file(File file_desc, uchar *buf, size_t buf_size,
                     Ha_clone_cbk *cbk_ctx, const std::string &fname,
                     const std::string &tname, size_t &copy_size,
                     bool is_log, bool send_file_name= true)
{
  DBUG_ASSERT(file_desc >= 0);
  DBUG_ASSERT(buf_size > 0);
  if (file_desc < 0 || !cbk_ctx || !buf || buf_size == 0)
  {
    copy_size= 0;
    my_error(ER_INTERNAL_ERROR, MYF(ME_ERROR_LOG),
             "ARIA SE: Clone send file invalid data");
    return ER_INTERNAL_ERROR;
  }

  uint64_t offset= send_file_name ? 0 : Descriptor::S_OFF_APPEND;
  bool read_all= (copy_size == 0);
  int err= 0;
  size_t copied_size= 0;
  auto chunk_size= read_all ? buf_size : std::min(buf_size, copy_size);

  while (size_t bytes_read= my_read(file_desc, buf, chunk_size, MY_WME))
  {
    if (bytes_read == size_t(-1))
    {
      my_printf_error(ER_IO_READ_ERROR, "Error: file %s read for table %s",
          ME_ERROR_LOG, fname.c_str(), tname.c_str());
      return ER_IO_READ_ERROR;
    }
    err= send_data(cbk_ctx, buf, bytes_read, offset,
                   send_file_name ? fname : "", is_log);
    if (err)
      break;
    copied_size+= bytes_read;

    if (!read_all)
    {
      if (copied_size >= copy_size)
      {
        DBUG_ASSERT(copy_size == copied_size);
        break;
      }
      auto size_left= copy_size - copied_size;
      chunk_size= std::min(chunk_size, size_left);
    }
    send_file_name= false;
  }
  if (!err && copied_size == 0)
    err= send_data(cbk_ctx, buf, 0, Descriptor::S_OFF_NO_DATA, fname,
                   is_log);
  copy_size= copied_size;
  return err;
}

class Table
{
 public:
  struct Partition
  {
    std::string m_file_path;
    File m_files[2]= {-1, -1};
    MY_STAT m_stats[2];
  };
  static constexpr const char *s_extns[]= {".MAI", ".MAD"};

  Table(std::string &db, std::string &table, std::string &frm_name,
        const char *file_path);
  ~Table();

  void add_partition(const Table &partition)
  {
    DBUG_ASSERT(m_partitioned);
    m_partitions.push_back(partition.m_partitions[0]);
  }

  int open(THD *thd, bool no_lock);
  int copy(Ha_clone_cbk *cbk_ctx);
  void close();

  std::string &get_db() { return m_db; }
  std::string &get_table() { return m_table; }
  std::string &get_version() { return m_version; }
  std::string &get_full_name() { return m_full_name; }
  bool is_partitioned() const { return m_partitioned; }

  bool is_online_backup_safe() const
  {
    DBUG_ASSERT(is_opened());
    return m_cap.online_backup_safe;
  }
  bool is_stats() const
  {
    return clone_common::is_stats_table(m_db.c_str(), m_table.c_str());
  }
  bool is_log() const
  {
    return clone_common::is_log_table(m_db.c_str(), m_table.c_str());
  }
  bool is_opened() const
  {
    return !m_partitions.empty() &&
           m_partitions[0].m_files[0] >= 0 &&
           m_partitions[0].m_files[1] >= 0;
  }

 private:
  std::string m_db;
  std::string m_table;
  std::string m_frm_name;
  std::string m_version;
  std::string m_full_name;

  bool m_partitioned= false;
  std::vector<Partition> m_partitions;
  ARIA_TABLE_CAPABILITIES m_cap;
};

Table::Table(std::string &db, std::string &table, std::string &frm_name,
    const char *file_path) :
    m_db(std::move(db)), m_table(std::move(table)),
    m_frm_name(std::move(frm_name))
{
  m_full_name.assign("`").append(m_db).append("`.`");
  m_full_name.append(m_table).append("`");

  if (std::strstr(file_path, "#P#"))
    m_partitioned= true;

  Partition partition;
  const char *ext_pos = std::strrchr(file_path, '.');
  partition.m_file_path.assign(file_path, ext_pos - file_path);
  m_partitions.push_back(std::move(partition));
}

int Table::open(THD *thd, bool no_lock)
{
  int error= 0;
  bool have_capabilities= false;
  File frm_file= -1;
  bool locked= false;

  if (!no_lock && clone_backup_lock(thd, m_db.c_str(), m_table.c_str()))
  {
    my_printf_error(ER_INTERNAL_ERROR,
        "Error on executing BACKUP LOCK for ARIA table %s", ME_ERROR_LOG,
        m_full_name.c_str());
    error= ER_INTERNAL_ERROR;
    goto exit;
  }
  else
    locked= !no_lock;
#ifndef DBUG_OFF
  if (strcmp(m_table.c_str(), "table_stats") == 0)
    DEBUG_SYNC_C("clone_backup_lock");
#endif

  for (Partition &partition : m_partitions)
  {
    for (size_t index= 0; index < 2; index++)
    {
      auto &extn= s_extns[index];
      std::string file_path= partition.m_file_path + extn;

      partition.m_files[index]= mysql_file_open(0, file_path.c_str(),
          O_RDONLY | O_SHARE | O_NOFOLLOW | O_CLOEXEC, MYF(MY_WME));
      if (partition.m_files[index] < 0)
      {
        my_printf_error(ER_CANT_OPEN_FILE,
            "Error on file %s open during %s ARIA table copy", ME_ERROR_LOG,
            file_path.c_str(), m_full_name.c_str());
        error= ER_CANT_OPEN_FILE;
        goto exit;
      }

      if (!my_stat(file_path.c_str(), &partition.m_stats[index], MYF(0)))
      {
        my_printf_error(ER_INTERNAL_ERROR,
            "Error: failed to get stat info for file %s of table %s",
            ME_ERROR_LOG, file_path.c_str(), m_full_name.c_str());
        error= ER_INTERNAL_ERROR;
        goto exit;
      }
    }
    if (!have_capabilities)
    {
      if ((error= aria_get_capabilities(partition.m_files[0], &m_cap)))
      {
        my_printf_error(ER_INTERNAL_ERROR,
          "Error: ARIA getting capability: %d", ME_ERROR_LOG, error);
        goto exit;
      }
      have_capabilities= true;
    }
  }
  frm_file= mysql_file_open(key_file_frm, (m_frm_name + ".frm").c_str(),
                            O_RDONLY | O_SHARE, MYF(0));
  if (frm_file < 0)
  {
    my_printf_error(ER_INTERNAL_ERROR,
        "Error on ARIA FRM file open: %s", ME_ERROR_LOG,
        (m_frm_name + ".frm").c_str());
    error= ER_INTERNAL_ERROR;
  }

exit:
  if (locked && clone_backup_unlock(thd))
  {
    my_printf_error(ER_INTERNAL_ERROR,
                    "Error on BACKUP UNLOCK for ARIA table %s",
                    ME_ERROR_LOG, m_full_name.c_str());
    error= ER_INTERNAL_ERROR;
  }
  if (frm_file >= 0)
  {
    m_version= clone_common::read_table_version_id(frm_file);
    mysql_file_close(frm_file, MYF(0));
  }
  if (error) close();
  return error;
}

Table::~Table()
{
  close();
}

void Table::close()
{
  for (Partition &partition : m_partitions)
  {
    for (size_t index= 0; index < 2; index++)
    {
      auto file_desc= partition.m_files[index];
      if (file_desc >= 0)
        mysql_file_close(partition.m_files[index], MYF(0));
      partition.m_files[index]= -1;
    }
  }
}

int Table::copy(Ha_clone_cbk *cbk_ctx)
{
  auto buf_size= static_cast<size_t>(m_cap.block_size);
  std::unique_ptr<uchar[]> buf(new uchar[buf_size]);
  int err= 0;

  for (const auto &part : m_partitions)
  {
    /* Loop two time for data and index file. */
    for (size_t index= 0; index < 2; index++)
    {
      size_t data_bytes= 0;
      auto &extn= s_extns[index];
      std::string file_path= part.m_file_path + extn;

      for (ulonglong block= 0;; block++)
      {
        size_t buf_len= buf_size;
        if (index)
          err= aria_read_data(part.m_files[index], &m_cap, block, buf.get(),
                              &buf_len);
        else
          err= aria_read_index(part.m_files[index], &m_cap, block, buf.get());
        if (err == HA_ERR_END_OF_FILE)
        {
          err= 0;
          break;
        }
        if (err)
        {
          my_printf_error(ER_IO_READ_ERROR, "Error: file %s read for table %s",
              ME_ERROR_LOG, file_path.c_str(), m_full_name.c_str());
          return ER_IO_READ_ERROR;
        }
        err= send_data(cbk_ctx, buf.get(), buf_len, Descriptor::S_OFF_APPEND,
                       (block == 0) ? file_path : "");
        if (err)
          return err;
        data_bytes+= buf_len;
      }
      my_printf_error(ER_CLONE_SERVER_TRACE, "ARIA SE: Copied file %s for "
                      "table %s, %zu bytes", MYF(ME_NOTE | ME_ERROR_LOG_ONLY),
                      file_path.c_str(), m_full_name.c_str(), data_bytes);
    }
  }
  return 0;
}

class Log_Files
{
public:
  /** Initialize by checking existing log files on the disk. */
  Log_Files(const char *datadir, uint32_t max_log_no, uint32_t min_log_no= 0);

  uint32_t first() const { return m_first; }
  uint32_t count() const { return m_count; }
  uint32_t last() const
  {
    DBUG_ASSERT(m_count > 0);
    return m_first + m_count - 1;
  }
  void report_found() const
  {
    if (m_count)
      sql_print_information("Found %u aria log files, minimum log number %u, "
                            "maximum log number %u", m_count, m_first, last());
  }
  bool check_if_missing(uint32_t logno) const
  {
    DBUG_ASSERT(logno > 0);
    return (!m_count || m_first > logno || last() < logno);
  }

  static std::string name_by_index(size_t log_num)
  {
    static constexpr const char *prefix= "aria_log.";
    std::string log_file;
    {
      std::stringstream ss;
      ss << std::setw(8) << std::setfill('0') << log_num;
      log_file.append(prefix).append(ss.str());
    }
    return log_file;
  }

  static std::string name(const char *datadir_path, size_t log_num)
  {
    std::string log_file(datadir_path);
    return log_file.append("/").append(name_by_index(log_num));
  }

 private:
  /** Check to see if a file exists.  Takes name of the file to check.
  @return true if file exists. */
  static bool file_exists(const char *filename)
  {
    MY_STAT stat_arg;
    auto stat= my_stat(filename, &stat_arg, MYF(0));
    return (stat != nullptr);
  }
  /*
    Skip all missing log files and find the greatest existing log file, or
    Skip all existing log files and find the greatest missing log file.

    @param datadir  - Search files in this directory
    @param start    - Start searching from this log number
    @param stop     - Search up to this point excluding stop
    @param kind     - true  - search for an existing file
                      false - search for a missing file.
    @returns        - (stop..start] - the greatest found log file
                                   of the searched kind
                    - 0  - if no log files of this kind
                           were found in the range (stop..start].
  */
  static uint32_t find_greatest(const char *datadir, uint32_t start,
                                uint32_t stop, bool kind)
  {
    for (uint32_t i= start; i > stop; i--)
    {
      if (file_exists(name(datadir, i).c_str()) == kind)
        return i;
    }
    return stop; // No log files of the searched kind were found
  }

  static uint32_t find_greatest_existing(const char *datadir, uint32_t start,
                                         uint32_t stop)
  {
    return find_greatest(datadir, start, stop, true);
  }

  static uint32_t find_greatest_missing(const char *datadir, uint32_t start,
                                        uint32_t stop)
  {
    return find_greatest(datadir, start, stop, false);
  }

 private:
  uint32_t m_first= 0;
  uint32_t m_count= 0;
};

Log_Files::Log_Files(const char *datadir, uint32_t max_log_no,
                     uint32_t min_log_no)
{
  auto end= find_greatest_existing(datadir, max_log_no, min_log_no);
  DBUG_ASSERT(end >= min_log_no);
  if (end == min_log_no + 1)
  {
    // Just the very one log file (aria_log.00000001 when min_log_no= 0) was found.
    m_first= min_log_no + 1;
    m_count= 1;
  }
  else if (end > min_log_no + 1)
  {
    // Multiple files were found
    m_first= find_greatest_missing(datadir, end - 1, min_log_no) + 1;
    m_count= 1 + end - m_first;
    return;
  }
  else
  {
    DBUG_ASSERT(end == min_log_no);
    // No log files were found at all
    m_first= 0;
    m_count= 0;
  }
}

class Job_Repository
{
 public:
  using Job= std::function<int(THD *, Ha_clone_cbk *, uint32_t, int)>;
  void add_one(Job &&job);
  void finish(int err, Ha_clone_stage stage);
  int consume(THD *thd, uint32_t thread_id, Ha_clone_cbk *cbk,
              Ha_clone_stage stage, int err);
  int wait_pending(THD *thd);
  Ha_clone_stage last_finished_stage();

 private:
  std::mutex m_mutex;
  std::condition_variable m_cv;
  std::queue<Job> m_jobs;
  bool m_finished[HA_CLONE_STAGE_MAX]= {false};
  int m_error= 0;
  uint32_t n_pending= 0;
};

int Job_Repository::wait_pending(THD *thd)
{
  auto cond_fn= [&]
  {
    return (n_pending == 0);
  };
  std::unique_lock<std::mutex> lock(m_mutex);
  /* We try consuming first and then come here. No more jobs can be added at
  this point. */
  DBUG_ASSERT(m_jobs.empty());
  uint32_t count= 0;
  constexpr uint32_t max_count= 300;
  while (n_pending && ++count < max_count)
  {
    m_cv.wait_for(lock, std::chrono::seconds(1), cond_fn);
    if (thd_killed(thd))
    {
      my_error(ER_QUERY_INTERRUPTED, MYF(ME_ERROR_LOG));
      m_error= ER_QUERY_INTERRUPTED;
      return m_error;
    }
  }
  if (n_pending)
  {
    my_printf_error(ER_STATEMENT_TIMEOUT,
        "ARIA SE: Clone Timeout(5 minutes) while waiting for jobs to finish",
        MYF(ME_ERROR_LOG));
    m_error= ER_STATEMENT_TIMEOUT;
  }
  return m_error;
}

void Job_Repository::add_one(Job &&job)
{
  std::unique_lock<std::mutex> lock(m_mutex);
  m_jobs.push(std::forward<Job>(job));
  ++n_pending;
  DBUG_ASSERT(n_pending >= m_jobs.size());
  lock.unlock();
  m_cv.notify_one();
}

void Job_Repository::finish(int err, Ha_clone_stage stage)
{
  std::unique_lock<std::mutex> lock(m_mutex);
  if (stage < HA_CLONE_STAGE_MAX)
    m_finished[stage]= true;
  if (err && !m_error)
    m_error= err;
  lock.unlock();
  m_cv.notify_all();
}

int Job_Repository::consume(THD *thd, uint32_t thread_id, Ha_clone_cbk *cbk,
                            Ha_clone_stage stage, int err)
{
  std::unique_lock<std::mutex> lock(m_mutex);
  while (!m_finished[stage] || !m_jobs.empty())
  {
    while (!m_jobs.empty())
    {
      auto job= std::move(m_jobs.front());
      m_jobs.pop();
      lock.unlock();
      /* Even after an error, we need to keep consuming all jobs added as jobs
      could hold table object ownership that needs to be freed. The input
      error would ensure we don't actually transfer any data after an error. */
      err= job(thd, cbk, thread_id, err);
      DBUG_ASSERT(n_pending > 0);
      --n_pending;
      if (thd_killed(thd))
      {
        my_error(ER_QUERY_INTERRUPTED, MYF(0));
        err= ER_QUERY_INTERRUPTED;
      }
      lock.lock();
    }
    if (m_error && !err)
    {
      my_error(ER_INTERNAL_ERROR, MYF(ME_ERROR_LOG),
               "ARIA SE: Clone error in concurrent task");
      err= m_error;
      break;
    }
    else if (err && !m_error)
    {
      m_error= err;
      break;
    }
    m_cv.wait_for(lock, std::chrono::seconds(1), [&]
    {
      return (m_finished[stage] || !m_jobs.empty() || m_error);
    });
  }
  return err;
}

Ha_clone_stage Job_Repository::last_finished_stage()
{
  Ha_clone_stage last_stage= HA_CLONE_STAGE_MAX;
  std::unique_lock<std::mutex> lock(m_mutex);
  auto stage= HA_CLONE_STAGE_CONCURRENT;
  while (stage < HA_CLONE_STAGE_MAX)
  {
    if (m_finished[stage] == false)
    {
      last_stage= stage;
      break;
    }
    stage= static_cast<Ha_clone_stage>(stage + 1);
  }
  lock.unlock();
  return last_stage;
}

using table_key_t= std::string;

inline table_key_t table_key(const std::string &db, const std::string &table)
{
  return std::string(db).append(".").append(table);
}

struct Thread_Context
{
  int open(const std::string &path, const std::string &file, uint64_t offset,
           bool log= false);
  int open_for_read(const std::string &path, const std::string &file,
                    bool log= false);
  void close();
  void close_log();

  uint32_t m_task_id= 0;
  File m_file= -1;
  File m_log_file= -1;
  std::string m_cur_data_file;
};

int Thread_Context::open_for_read(const std::string &path,
                                  const std::string &file, bool log)
{
  log ? close_log() : close();
  auto &cur_file= log ? m_log_file : m_file;

  char fullpath[FN_REFLEN];
  fn_format(fullpath, file.c_str(), path.c_str(), "", MYF(MY_RELATIVE_PATH));

  int open_flags= O_RDONLY | O_SHARE;
  cur_file= mysql_file_open(0, fullpath, open_flags, MYF(0));

  if (cur_file < 0)
  {
    cur_file= -1;
    my_error(ER_CANT_OPEN_FILE, MYF(ME_ERROR_LOG), fullpath, my_errno);
    return ER_CANT_OPEN_FILE;
  }
  return 0;
}

int Thread_Context::open(const std::string &path, const std::string &file,
                         uint64_t offset, bool log)
{
  /* Close previous file if there. */
  log ? close_log() : close();
  auto &cur_file= log ? m_log_file : m_file;

  char fullpath[FN_REFLEN];
  fn_format(fullpath, file.c_str(), path.c_str(), "", MYF(MY_RELATIVE_PATH));

  size_t dirpath_len= 0;
  char dirpath[FN_REFLEN];
  dirname_part(dirpath, fullpath, &dirpath_len);

  /* Make schema directory path and create file, if needed. */
  if (my_mkdir(dirpath, 0777, MYF(0)) >= 0 || my_errno == EEXIST)
  {
    int open_flags= O_WRONLY | O_BINARY;

    if (offset == Descriptor::S_OFF_APPEND)
      open_flags|= O_APPEND;
    else
      DBUG_ASSERT(offset == Descriptor::S_OFF_NO_DATA || !offset);

    cur_file= mysql_file_open(0, fullpath, open_flags, MYF(0));
    if (cur_file < 0)
    {
      open_flags|= O_CREAT;
      cur_file= mysql_file_open(0, fullpath, open_flags, MYF(0));
    }
  }
  if (cur_file < 0)
  {
    cur_file= -1;
    my_error(ER_CANT_OPEN_FILE, MYF(ME_ERROR_LOG), fullpath, my_errno);
    return ER_CANT_OPEN_FILE;
  }
  if (!log)
    m_cur_data_file.assign(file);
  return 0;
}

void Thread_Context::close_log()
{
  if (m_log_file < 0)
    return;
  mysql_file_close(m_log_file, MYF(0));
  m_log_file= -1;
}

void Thread_Context::close()
{
  if (m_file < 0)
    return;
  mysql_file_close(m_file, MYF(0));
  m_file= -1;
}

class Clone_Handle
{
 public:
  Clone_Handle(bool is_copy, const Locator *ref_loc, const char *datadir,
      uint32_t index) : m_is_copy(is_copy), m_loc(ref_loc, index, is_copy),
      m_data_dir(datadir ? datadir : "."), m_log_dir(maria_data_root) {}

  void set_error(int err);
  int check_error(THD *thd);

  int clone_low(THD *thd, uint32_t task_id, Ha_clone_stage stage,
		Ha_clone_cbk *cbk);
  int clone(THD *thd, uint32_t task_id, Ha_clone_stage stage,
            Ha_clone_cbk *cbk);
  int apply(THD *thd, uint32_t task_id, Ha_clone_cbk *cbk);

  size_t attach();
  bool detach(size_t id);

  Locator &get_locator() { return m_loc; }
  static constexpr size_t S_MAX_TASKS= 128;

  bool max_task_reached() const
  {
    DBUG_ASSERT(m_next_task <= S_MAX_TASKS);
    return m_next_task >= S_MAX_TASKS;
  }

 private:
  int scan(bool no_lock);
  int copy_offline_tables(const std::unordered_set<std::string> &exclude_tables,
                          bool no_lock, bool copy_stats);
  int copy_log_tail(THD *thd, Ha_clone_cbk *cbk_ctx, bool finalize);

  int copy_table_job(Table *table_ptr, bool online_only, bool copy_stats,
                     bool no_lock, THD *thd, Ha_clone_cbk *cbk, uint32_t thread_id,
                     int in_error);

  int copy_file_job(std::string *file_name_ptr, bool is_log, THD *thd,
		    Ha_clone_cbk *cbk, uint32_t thread_id, int in_error);

  int copy_partial_tail(Ha_clone_cbk *cbk_ctx);

  int copy_finish_tail(Ha_clone_cbk *cbk_ctx);

 private:
  bool m_is_copy= true;
  /** Number of threads attached; Protected by Clone_Sys::mutex_ */
  size_t m_num_threads= 0;
  size_t m_next_task= 0;
  int m_error= 0;

  Locator m_loc;
  std::string m_data_dir;
  std::string m_log_dir;

  std::array<Thread_Context, S_MAX_TASKS> m_thread_ctxs;
  Job_Repository m_jobs;

  std::mutex m_offline_tables_mutex;
  std::vector<std::unique_ptr<Table>> m_offline_tables;

  size_t m_last_log_num= 0;
  size_t m_last_log_offset= 0;
};

size_t Clone_Handle::attach()
{
  /* ID is the index into the m_thread_ctxs vector. */
  auto id= m_next_task++;
  DBUG_ASSERT(id < S_MAX_TASKS);

  auto &ctx= m_thread_ctxs[id];
  ctx.m_task_id= static_cast<uint32_t>(id);
  DBUG_ASSERT(ctx.m_file == -1);

  m_num_threads++;
  DBUG_ASSERT(m_thread_ctxs.size() >= m_num_threads);

  return id;
}

bool Clone_Handle::detach(size_t id)
{
  auto &ctx= m_thread_ctxs[id];
  ctx.close();
  ctx.close_log();
  DBUG_ASSERT(m_num_threads > 0);
  return (0 == --m_num_threads);
}

int Clone_Handle::copy_file_job(std::string *file_name_ptr, bool is_log,
                                THD *thd, Ha_clone_cbk *cbk, uint32_t, int in_error)
{
  std::unique_ptr<std::string> file_name(file_name_ptr);
  int err= in_error;
  if (err)
    return err;

  std::string file_path;
  if (is_log)
  {
    file_path.assign(m_log_dir);
    if (file_path.back() != FN_LIBCHAR)
      file_path+= FN_LIBCHAR;
  }
  file_path.append(*file_name);

  File file= mysql_file_open(0, file_path.c_str(), O_RDONLY | O_SHARE,
                             MYF(0));
  if (file < 0)
  {
    my_printf_error(ER_CANT_OPEN_FILE, "Error on opening file: %s",
                    MYF(ME_ERROR_LOG), file_name->c_str());
    err= ER_CANT_OPEN_FILE;
  }
  else
  {
    size_t copy_size= 0;
    static const size_t buf_size = 10 * 1024 * 1024;
    std::unique_ptr<uchar[]> buf= std::make_unique<uchar[]>(buf_size);

    err= send_file(file, buf.get(), buf_size, cbk, (*file_name), "",
                   copy_size, is_log);
    mysql_file_close(file, MYF(0));

    my_printf_error(ER_CLONE_SERVER_TRACE, "ARIA SE: Copied complete redo log "
      "file %s of size %zu bytes", MYF(ME_NOTE | ME_ERROR_LOG_ONLY),
      file_name->c_str(), copy_size);
  }
  return err;
}

int Clone_Handle::copy_table_job(Table *table_ptr, bool online_only,
                                 bool copy_stats, bool no_lock,
                                 THD *thd, Ha_clone_cbk *cbk, uint32_t,
                                 int in_error)
{
  std::unique_ptr<Table> table(table_ptr);
  if (in_error)
    return in_error;

  int err= table->open(thd, no_lock);
  if (err)
    return err;

  bool is_online= table->is_online_backup_safe();
  bool is_stats= table->is_stats();
  bool need_copy= (!online_only || is_online) && (copy_stats || !is_stats);

  if (need_copy)
    err= table->copy(cbk);

  table->close();

  if (!need_copy)
  {
    std::lock_guard<std::mutex> lock(m_offline_tables_mutex);
    m_offline_tables.push_back(std::move(table));
    return 0;
  }

#ifndef DBUG_OFF
if (strcmp(table->get_table().c_str(), "t_dml") == 0)
  DEBUG_SYNC_C("after_aria_table_copy_t_dml");
#endif /* DBUG_OFF */

  /* TODO: Post Copy Hook for DDL */
  // if (!err && m_table_post_copy_hook)
  //   m_table_post_copy_hook(table->get_db(), table->get_table(),
  //                          table->get_version());
  return err;
}

int Clone_Handle::scan(bool no_lock)
{
  auto ctrl_file_name= std::make_unique<std::string>("aria_log_control");

  using namespace std::placeholders;
  m_jobs.add_one(std::bind(&Clone_Handle::copy_file_job, this,
                           ctrl_file_name.release(), true, _1, _2, _3, _4));

  my_printf_error(ER_CLONE_SERVER_TRACE, "ARIA SE: Start scanning engine table"
                  "s, need backup locks: %d",
                  MYF(ME_NOTE | ME_ERROR_LOG_ONLY), no_lock);
#ifndef EMBEDDED_LIBRARY
  std::set<std::string> ext_list= {".MAD"};
  std::unordered_map<std::string, std::unique_ptr<Table>> partitioned_tables;

  clone_common::foreach_file_in_dir(m_data_dir,
                                    [&](const fsys::path& file_path)
  {
    const char* fpath= nullptr;
#ifdef _WIN32
    std::wstring wstr= file_path.wstring();
    int size= WideCharToMultiByte(CP_UTF8, 0, &wstr[0],
                                  (int)wstr.size(), nullptr,
                                  0, nullptr, nullptr);
    std::string fil_path(size, 0);
    WideCharToMultiByte(CP_UTF8, 0, &wstr[0],
                        (int)wstr.size(), &fil_path[0],
                        size, nullptr, nullptr);
    fpath= fil_path.c_str();
#else /* _WIN32 */
    fpath= file_path.c_str();
#endif /* _WIN32 */

    /* TODO: Partial Backup */
    // if (check_if_skip_table(file_path))
    // {
    //   my_printf_error(ER_CLONE_SERVER_TRACE, "ARIA SE: Skipping %s.",
    //                   MYF(ME_NOTE | ME_ERROR_LOG_ONLY), file_path);
    //   return;
    // }
    auto db_table_fs=
      clone_common::convert_filepath_to_tablename(fpath);
    auto tk= table_key(std::get<0>(db_table_fs), std::get<1>(db_table_fs));

    auto table= std::make_unique<Table>(std::get<0>(db_table_fs),
      std::get<1>(db_table_fs), std::get<2>(db_table_fs), fpath);

    if (table->is_log())
      return;

    if (table->is_partitioned())
    {
      auto table_it= partitioned_tables.find(table->get_full_name());
      if (table_it == partitioned_tables.end())
        partitioned_tables[table->get_full_name()]= std::move(table);
      else
        table_it->second->add_partition(*table);
      return;
    }
    using namespace std::placeholders;
    m_jobs.add_one(std::bind(&Clone_Handle::copy_table_job, this,
        table.release(), true, false, no_lock,_1, _2, _3, _4));
  }, ext_list);

  for (auto &table_it : partitioned_tables)
  {
    m_jobs.add_one(std::bind(&Clone_Handle::copy_table_job, this,
        table_it.second.release(), true, false, no_lock, _1, _2, _3, _4));
  }

  auto horizon= translog_get_horizon();
  uint32_t last_file_num= LSN_FILE_NO(horizon);

  my_printf_error(ER_CLONE_SERVER_TRACE, "ARIA SE: Start scanning engine redo"
                  "logs, last log number: %u",
                  MYF(ME_NOTE | ME_ERROR_LOG_ONLY), last_file_num);

  Log_Files logs(m_log_dir.c_str(), last_file_num);

  DEBUG_SYNC_C("after_scanning_log_files");

  for (auto i= logs.first(); i < logs.last(); ++i)
  {
    auto log_file= std::make_unique<std::string>(Log_Files::name_by_index(i));
    m_jobs.add_one(std::bind(&Clone_Handle::copy_file_job, this,
                   log_file.release(), true, _1, _2, _3, _4));
  }
  m_last_log_num= logs.last();
  m_last_log_offset= 0;
  my_printf_error(ER_CLONE_SERVER_TRACE, "ARIA SE: Stop scanning engine "
                  "tables", MYF(ME_NOTE | ME_ERROR_LOG_ONLY));
#endif /* EMBEDDED_LIBRARY */
  return 0;
}

int Clone_Handle::copy_offline_tables(
    const std::unordered_set<std::string> &exclude_tables,
    bool no_lock, bool copy_stats)
{
  std::vector<std::unique_ptr<Table>> ignored_tables;
  for(;;)
  {
    std::unique_lock<std::mutex> lock(m_offline_tables_mutex);
    if (m_offline_tables.empty())
      break;
    auto table= std::move(m_offline_tables.back());
    m_offline_tables.pop_back();
    lock.unlock();
    auto tkey= table_key(table->get_db(), table->get_table());
    if ((!exclude_tables.empty() && exclude_tables.count(tkey)) ||
        (!copy_stats && table->is_stats()))
    {
      ignored_tables.push_back(std::move(table));
      continue;
    }
    using namespace std::placeholders;
    m_jobs.add_one(std::bind(&Clone_Handle::copy_table_job, this,
        table.release(), false, copy_stats, no_lock, _1, _2, _3, _4));
  }
  if (!ignored_tables.empty())
  {
    std::lock_guard<std::mutex> lock(m_offline_tables_mutex);
    m_offline_tables= std::move(ignored_tables);
  }
  return 0;
}

int Clone_Handle::copy_finish_tail(Ha_clone_cbk *cbk_ctx)
{
  int err= 0;
  DBUG_ASSERT(m_last_log_num > 0);
  if (m_last_log_num == 0)
    return 0;

  auto &ctx= m_thread_ctxs[0];
  auto log_file=
      std::make_unique<std::string>(Log_Files::name_by_index(m_last_log_num));

  /* If the tail log file is not opened yet, send the entire log file. */
  if (ctx.m_log_file == -1)
    return copy_file_job(log_file.release(), true, nullptr, cbk_ctx, 0, 0);

  /* Send the rest of the log file. */
  size_t copy_size= 0;
  static const size_t buf_size= 1024 * 1024;
  std::unique_ptr<uchar[]> buf= std::make_unique<uchar[]>(buf_size);
  err= send_file(ctx.m_log_file, buf.get(), buf_size, cbk_ctx, (*log_file), "",
                 copy_size, true, false);
  my_printf_error(ER_CLONE_SERVER_TRACE, "ARIA SE: Copied rest of the redo log"
    " file %s of size %zu bytes from offset %zu bytes",
    MYF(ME_NOTE | ME_ERROR_LOG_ONLY), log_file->c_str(), copy_size,
    m_last_log_offset);

  ctx.close_log();
  m_last_log_num= 0;
  m_last_log_offset= 0;

  if (err)
    return err;

  /* Send the header again to update LSN. */
  if ((err= ctx.open_for_read(m_log_dir, (*log_file), true)))
    return err;
  copy_size= LOG_HEADER_DATA_SIZE;
  err= send_file(ctx.m_log_file, buf.get(), buf_size, cbk_ctx, (*log_file), "",
                 copy_size, true);
  my_printf_error(ER_CLONE_SERVER_TRACE, "ARIA SE: Updated header of redo log"
    " file %s of size %zu bytes", MYF(ME_NOTE | ME_ERROR_LOG_ONLY),
    log_file->c_str(), copy_size);
  ctx.close_log();
  m_last_log_num= 0;
  m_last_log_offset= 0;
  return err;
}

template <typename T>
static T align_down(T value, T alignment)
{
  DBUG_ASSERT(alignment != 0);
  DBUG_ASSERT((alignment & (alignment - 1)) == 0);
  return value & ~(alignment - 1);
}

int Clone_Handle::copy_partial_tail(Ha_clone_cbk *cbk_ctx)
{
  int err= 0;
  DBUG_ASSERT(m_last_log_num > 0);
  if (m_last_log_num == 0)
    return err;
  auto log_file=
      std::make_unique<std::string>(Log_Files::name_by_index(m_last_log_num));
  auto &ctx= m_thread_ctxs[0];

  bool send_file_name= false;
  if (ctx.m_log_file < 0)
  {
    send_file_name= true;
    if ((err= ctx.open_for_read(m_log_dir, (*log_file), true)))
      return err;
  }
  MY_STAT stat_info;
  memset(&stat_info, 0, sizeof(MY_STAT));
  if (my_fstat(ctx.m_log_file, &stat_info, MYF(0)))
  {
    my_printf_error(ER_INTERNAL_ERROR, "Error: failed to get stat info for "
                    "ARIA log file %s", ME_ERROR_LOG, log_file->c_str());
    return ER_INTERNAL_ERROR;
  }
  size_t file_size= static_cast<size_t>(stat_info.st_size);

  if (file_size <= m_last_log_offset)
  {
    DBUG_ASSERT(file_size == m_last_log_offset);
    return 0;
  }
  /* Copy without the last page, which can be rewritten. */
  auto copy_size= static_cast<size_t>(file_size - m_last_log_offset);
  copy_size= align_down(copy_size, static_cast<size_t>(TRANSLOG_PAGE_SIZE));
  if (copy_size <= TRANSLOG_PAGE_SIZE)
    return 0;
  copy_size-= TRANSLOG_PAGE_SIZE;
  DBUG_ASSERT(copy_size > 0);

  static const size_t buf_size= 1024 * 1024;
  std::unique_ptr<uchar[]> buf= std::make_unique<uchar[]>(buf_size);
  err= send_file(ctx.m_log_file, buf.get(), buf_size, cbk_ctx, (*log_file), "",
                 copy_size, true, send_file_name);
  if (!err)
    my_printf_error(ER_CLONE_SERVER_TRACE, "ARIA SE: Copied partial redo log "
      "file %s of size %zu bytes from offset %zu bytes",
      MYF(ME_NOTE | ME_ERROR_LOG_ONLY), log_file->c_str(), copy_size,
      m_last_log_offset);

  m_last_log_offset+= copy_size;
  return err;
}

int Clone_Handle::copy_log_tail(THD *thd, Ha_clone_cbk *cbk_ctx, bool finalize)
{
  int err= 0;
  if (finalize && (err= m_jobs.wait_pending(thd)))
    return err;

  /* Check for new log files added. */
  auto horizon= translog_get_horizon();
  uint32_t last_file_num= LSN_FILE_NO(horizon);
  Log_Files logs(m_log_dir.c_str(), last_file_num,
		 static_cast<uint32_t>(m_last_log_num));

  if (!logs.count())
  {
    /* No new log files. */
    err= finalize ? copy_finish_tail(cbk_ctx) : copy_partial_tail(cbk_ctx);
    return err;
  }
  /* There are more log files added. Finish the current one and continue
  with the rest. */
  if ((err= copy_finish_tail(cbk_ctx)))
    return err;

  for (auto i= logs.first(); i < logs.last(); ++i)
  {
    auto log_file= std::make_unique<std::string>(Log_Files::name_by_index(i));
    if ((err= copy_file_job(log_file.release(), true, nullptr, cbk_ctx, 0, 0)))
      return err;
  }
  /* Set new tail log. */
  m_last_log_num= logs.last();
  m_last_log_offset= 0;
  err= finalize ? copy_finish_tail(cbk_ctx) : copy_partial_tail(cbk_ctx);
  return err;
}

class Clone_Sys
{
 public:
  int start(bool is_copy, bool attach, Clone_Handle *&clone_hdl, uint32_t &id,
            const Locator *ref_loc= nullptr, const char *data_dir= nullptr);
  int stop(bool is_copy, Clone_Handle *&clone_hdl, uint32_t task_id);

  Clone_Handle *find(const Locator *in_loc, bool is_copy);
  Clone_Handle *get(uint32_t index, bool is_copy);

  uint32_t next_id() { return m_next_clone_id++; }

  static constexpr uint32_t S_MAX_CLONE= 1;
  static std::mutex mutex_;
 private:
  std::mutex m_mutex;
  uint32_t m_next_clone_id= 1;

  std::array<Clone_Handle*, S_MAX_CLONE> m_copy_clones;
  std::array<Clone_Handle*, S_MAX_CLONE> m_apply_clones;
};
inline std::mutex Clone_Sys::mutex_;

int Clone_Sys::start(bool is_copy, bool attach, Clone_Handle *&clone_hdl,
                     uint32_t &id, const Locator *ref_loc,
                     const char *data_dir)
{
  if (!attach)
  {
    /* Create a new clone handle. */
    auto &clones= is_copy ? m_copy_clones : m_apply_clones;

    uint32_t index= 0;
    for (auto clone_ : clones)
    {
      if (clone_ == nullptr)
        break;
      ++index;
    }
    if (index >= S_MAX_CLONE)
    {
      /* Too many active clones .*/
      my_error(ER_CLONE_TOO_MANY_CONCURRENT_CLONES, MYF(ME_ERROR_LOG),
               S_MAX_CLONE);
      return ER_CLONE_TOO_MANY_CONCURRENT_CLONES;
    }
    clones[index]= new(std::nothrow) Clone_Handle(is_copy, ref_loc, data_dir,
                                                  index);
    clone_hdl= clones[index];
  }
  if (!clone_hdl)
  {
    DBUG_ASSERT(attach);
    /* Operation has finished already */
    my_error(ER_INTERNAL_ERROR, MYF(ME_ERROR_LOG),
             "ARIA SE: Clone add task refers non-existing clone");
    /* No active clone to attach to. */
    return ER_INTERNAL_ERROR;
  }

  if (clone_hdl->max_task_reached())
  {
    DBUG_ASSERT(attach);
    my_error(ER_INTERNAL_ERROR, MYF(ME_ERROR_LOG),
             "ARIA SE: Maximum Tasks reached");
    return ER_INTERNAL_ERROR;
  }
  id= static_cast<uint32_t>(clone_hdl->attach());
  return 0;
}

int Clone_Sys::stop(bool is_copy, Clone_Handle *&clone_hdl, uint32_t task_id)
{
  bool last= clone_hdl->detach(static_cast<size_t>(task_id));
  if (last)
  {
    auto &clones= is_copy ? m_copy_clones : m_apply_clones;
    auto index= clone_hdl->get_locator().index();
    DBUG_ASSERT(clones[index] == clone_hdl);
    clones[index]= nullptr;
    delete clone_hdl;
    clone_hdl= nullptr;
  }
  return 0;
}

Clone_Handle *Clone_Sys::find(const Locator *in_loc,  bool is_copy)
{
  if (!in_loc)
    return nullptr;
  auto &clones= is_copy ? m_copy_clones : m_apply_clones;

  for (auto clone_hdl : clones)
  {
    if (!clone_hdl)
      continue;

    auto& loc= clone_hdl->get_locator();
    if (loc == *in_loc)
      return clone_hdl;
  }
  return nullptr;
}

Clone_Handle *Clone_Sys::get(uint32_t index, bool is_copy)
{
  if (index > S_MAX_CLONE)
    return nullptr;
  auto &clones= is_copy ? m_copy_clones : m_apply_clones;
  return clones[index];
}

static Clone_Sys clone_system;
static Clone_Sys *const clone_sys= &clone_system;

Locator::Locator(const Locator *ref_loc, uint32_t clone_index, bool is_copy)
{
  m_version= S_CUR_VERSION;
  if (ref_loc && m_version > ref_loc->m_version)
    m_version= ref_loc->m_version;
  m_index= clone_index;

  uint32_t ref_id= ref_loc ? ref_loc->m_clone_id : 0;
  m_clone_id= is_copy ? clone_sys->next_id() : ref_id;
  serialize();
}

int Clone_Handle::check_error(THD *thd)
{
  if (thd_killed(thd))
  {
    my_error(ER_QUERY_INTERRUPTED, MYF(ME_ERROR_LOG));
    set_error(ER_QUERY_INTERRUPTED);
  }
  const std::lock_guard<std::mutex> lock(Clone_Sys::mutex_);
  return m_error;
}

void Clone_Handle::set_error(int err)
{
  if (err == 0)
    return;
  std::unique_lock<std::mutex> lock(Clone_Sys::mutex_);
  if (m_error)
    return;
  m_error= err;
  lock.unlock();

  if (m_is_copy)
    m_jobs.finish(err, HA_CLONE_STAGE_MAX);
}

int Clone_Handle::apply(THD *thd, uint32_t task_id, Ha_clone_cbk *cbk)
{
  uint32_t desc_len= 0;
  auto desc_buf= cbk->get_data_desc(&desc_len);

  Descriptor clone_desc(desc_buf, desc_len);
  auto &ctx= m_thread_ctxs[task_id];

  auto [file_name, offset]= clone_desc.get_file_info();
  /* Currently the write is append only or over-write */
  DBUG_ASSERT(!offset || offset == Descriptor::S_OFF_APPEND ||
         offset == Descriptor::S_OFF_NO_DATA);

  bool is_log= clone_desc.is_log();
  int err= 0;
  if (!file_name.empty() &&
      (err= ctx.open(m_data_dir, file_name, offset, is_log)))
    return err;

  if (offset == Descriptor::S_OFF_NO_DATA)
  {
    is_log ? ctx.close_log() : ctx.close();
    return 0;
  }
  auto &cur_file= is_log ? ctx.m_log_file : ctx.m_file;
  Ha_clone_file file;
  DBUG_ASSERT(cur_file >= 0);
  if (cur_file < 0)
  {
    my_error(err, MYF(ME_ERROR_LOG),
             "ARIA SE: Cannot apply data- missing file name");
    return ER_INTERNAL_ERROR;
  }
#ifdef _WIN32
  file.type= Ha_clone_file::FILE_HANDLE;
  file.file_handle= static_cast<void *>(my_get_osfhandle(cur_file));
#else
  file.type= Ha_clone_file::FILE_DESC;
  file.file_desc= cur_file;
#endif /* _WIN32 */

  cbk->set_os_buffer_cache();
  return cbk->apply_file_cbk(file);
}

int Clone_Handle::clone_low(THD *thd, uint32_t task_id, Ha_clone_stage stage,
                     Ha_clone_cbk *cbk)
{
  int err= 0;
  bool copy_tail= false;
  std::unordered_set<std::string> tables_in_use;
  switch (stage)
  {
    case HA_CLONE_STAGE_CONCURRENT:
      if (task_id != 0)
        break;
      err= scan(false);
      copy_tail= true;
      break;
    case HA_CLONE_STAGE_NT_DML_BLOCKED:
      if (task_id != 0)
        break;
      /* TODO: get_tables_in_use() : "SHOW OPEN TABLES WHERE In_use = 1" */
      err= copy_offline_tables(tables_in_use, false, false);
      copy_tail= true;
      break;
    case HA_CLONE_STAGE_DDL_BLOCKED:
      if (task_id != 0)
        break;
      tables_in_use.clear();
      err= copy_offline_tables(tables_in_use, true, false);
      copy_tail= true;
      break;
    case HA_CLONE_STAGE_SNAPSHOT:
      if (task_id != 0)
        break;
      tables_in_use.clear();
      err= copy_offline_tables(tables_in_use, true, true);
      copy_tail= true;
      break;
    case HA_CLONE_STAGE_END:
      break;
    case HA_CLONE_STAGE_MAX:
     DBUG_ASSERT(false);
     err= ER_INTERNAL_ERROR;
     my_error(err, MYF(ME_ERROR_LOG), "ARIA SE: Invalid Execution Stage");
     break;
  }
  if (task_id == 0)
    m_jobs.finish(err, stage);
  err= m_jobs.consume(thd, task_id, cbk, stage, err);
  set_error(err);

  if (!err && copy_tail)
  {
    DBUG_ASSERT(task_id == 0);
    err= copy_log_tail(thd, cbk, stage == HA_CLONE_STAGE_SNAPSHOT);
  }
  return err;
}

int Clone_Handle::clone(THD *thd, uint32_t task_id, Ha_clone_stage stage,
                        Ha_clone_cbk *cbk)
{
  int err= 0;
  Ha_clone_stage cur_stage= m_jobs.last_finished_stage();
  while (!err && cur_stage <= stage)
  {
    err= clone_low(thd, task_id, cur_stage, cbk);
    cur_stage= static_cast<Ha_clone_stage>(cur_stage + 1);
  }
  return err;
}
} // namespace aria_engine

#ifndef EMBEDDED_LIBRARY
static void clone_get_capability(Ha_clone_flagset &flags)
{
  flags.reset();
  flags.set(HA_CLONE_BLOCKING);
  flags.set(HA_CLONE_MULTI_TASK);
}

static int clone_begin(THD *, const uchar *&loc, uint &loc_len,
                       uint &task_id, Ha_clone_type, Ha_clone_mode mode)
{
  aria_engine::Locator *in_loc= nullptr;
  if (loc)
    in_loc= new(std::nothrow) aria_engine::Locator(loc, loc_len);
  int err= 0;

  const std::lock_guard<std::mutex> lock(aria_engine::Clone_Sys::mutex_);
  auto clone_hdl= aria_engine::clone_sys->find(in_loc, true);

  switch (mode)
  {
    case HA_CLONE_MODE_START:
      err= aria_engine::clone_sys->start(true, false, clone_hdl, task_id,
                                           in_loc);
      break;
    case HA_CLONE_MODE_ADD_TASK:
      err= aria_engine::clone_sys->start(true, true, clone_hdl, task_id,
                                           in_loc);
      break;
    case HA_CLONE_MODE_RESTART:
      err=ER_NOT_SUPPORTED_YET;
      my_error(ER_NOT_SUPPORTED_YET, MYF(ME_ERROR_LOG),
               "ARIA SE: Clone Restart after network failure");
      break;
    case HA_CLONE_MODE_VERSION:
    case HA_CLONE_MODE_MAX:
      err= ER_INTERNAL_ERROR;
      my_error(err, MYF(ME_ERROR_LOG), "ARIA SE: Clone Begin Invalid Mode");
      DBUG_ASSERT(false);
  }
  if (!err && clone_hdl)
  {
    auto &locator= clone_hdl->get_locator();
    std::tie(loc, loc_len)= locator.get_locator();
  }
  delete in_loc;
  return err;
}

static int clone_copy(THD *thd, const uchar *loc, uint loc_len, uint task_id,
                      Ha_clone_stage stage, Ha_clone_cbk *cbk)
{
  DBUG_ASSERT(loc);
  std::unique_ptr<aria_engine::Locator>
    in_loc(new(std::nothrow) aria_engine::Locator(loc, loc_len));

  auto clone_hdl= aria_engine::clone_sys->get(in_loc->index(), true);
  int err= clone_hdl ? clone_hdl->check_error(thd) : 0;

  if (!clone_hdl || err != 0)
    return err;

  return clone_hdl->clone(thd, task_id, stage, cbk);
}

static int clone_ack(THD *, const uchar *loc, uint loc_len,
                     uint, int in_err, Ha_clone_cbk *)
{
  DBUG_ASSERT(loc);
  std::unique_ptr<aria_engine::Locator>
    in_loc(new(std::nothrow) aria_engine::Locator(loc, loc_len));
  auto clone_hdl= aria_engine::clone_sys->get(in_loc->index(), true);
  DBUG_ASSERT(clone_hdl);
  if (!clone_hdl)
    return 0;
  clone_hdl->set_error(in_err);
  return 0;
}

static int clone_end(THD *, const uchar *loc, uint loc_len, uint task_id,
                     int in_err)
{
  DBUG_ASSERT(loc);
  std::unique_ptr<aria_engine::Locator>
    in_loc(new(std::nothrow) aria_engine::Locator(loc, loc_len));
  auto clone_hdl= aria_engine::clone_sys->get(in_loc->index(), true);

  DBUG_ASSERT(clone_hdl);
  if (!clone_hdl)
    return 0;
  clone_hdl->set_error(in_err);

  const std::lock_guard<std::mutex> lock(aria_engine::Clone_Sys::mutex_);
  return aria_engine::clone_sys->stop(true, clone_hdl, task_id);
}

static int clone_apply_begin(THD *, const uchar *&loc,
                             uint &loc_len, uint &task_id, Ha_clone_mode mode,
                             const char *data_dir)
{
  aria_engine::Locator *in_loc= nullptr;
  if (loc)
    in_loc= new(std::nothrow) aria_engine::Locator(loc, loc_len);
  int err= 0;

  const std::lock_guard<std::mutex> lock(aria_engine::Clone_Sys::mutex_);
  auto clone_hdl= aria_engine::clone_sys->find(in_loc, false);

  switch (mode)
  {
    case HA_CLONE_MODE_VERSION:
    case HA_CLONE_MODE_START:
      DBUG_ASSERT(!clone_hdl);
      err= aria_engine::clone_sys->start(false, false, clone_hdl, task_id,
                                           in_loc, data_dir);
      task_id= 0;
      break;
    case HA_CLONE_MODE_ADD_TASK:
      err= aria_engine::clone_sys->start(false, true, clone_hdl, task_id,
                                           in_loc);
      break;
    case HA_CLONE_MODE_RESTART:
      err=ER_NOT_SUPPORTED_YET;
      my_error(ER_NOT_SUPPORTED_YET, MYF(ME_ERROR_LOG),
               "ARIA SE: Clone Restart after network failure");
      break;
    case HA_CLONE_MODE_MAX:
      err= ER_INTERNAL_ERROR;
      my_error(err, MYF(ME_ERROR_LOG), "ARIA SE: Clone Begin Invalid Mode");
      DBUG_ASSERT(false);
  }

  /* While attaching tasks, don't overwrite the source locator. */
  if (!err && clone_hdl && mode != HA_CLONE_MODE_ADD_TASK)
  {
    auto &locator= clone_hdl->get_locator();
    std::tie(loc, loc_len)= locator.get_locator();
  }
  delete in_loc;
  return err;
}

static int clone_apply(THD *thd, const uchar *loc,
                       uint loc_len, uint task_id, int in_err,
                       Ha_clone_cbk *cbk)
{
  DBUG_ASSERT(loc);
  std::unique_ptr<aria_engine::Locator>
    in_loc(new(std::nothrow) aria_engine::Locator(loc, loc_len));

  auto clone_hdl= aria_engine::clone_sys->get(in_loc->index(), false);

  DBUG_ASSERT(in_err != 0 || cbk != nullptr);
  if (clone_hdl && (in_err != 0 || cbk == nullptr))
  {
    clone_hdl->set_error(in_err);
    my_printf_error(ER_CLONE_CLIENT_TRACE, "ARIA SE: Set Error Code %d",
        MYF(ME_NOTE | ME_ERROR_LOG_ONLY), in_err);
    return 0;
  }

  int err= clone_hdl ? clone_hdl->check_error(thd) : 0;
  if (!clone_hdl || err != 0)
    return err;

  err= clone_hdl->apply(thd, task_id, cbk);
  clone_hdl->set_error(err);
  return err;
}

static int clone_apply_end(THD *, const uchar *loc, uint loc_len,
                           uint task_id, int in_err)
{
  DBUG_ASSERT(loc);
  std::unique_ptr<aria_engine::Locator>
    in_loc(new(std::nothrow) aria_engine::Locator(loc, loc_len));
  auto clone_hdl= aria_engine::clone_sys->get(in_loc->index(), false);
  DBUG_ASSERT(clone_hdl);
  clone_hdl->set_error(in_err);

  const std::lock_guard<std::mutex> lock(aria_engine::Clone_Sys::mutex_);
  return aria_engine::clone_sys->stop(false, clone_hdl, task_id);
}
#endif /* !EMBEDDED_LIBRARY */

void init_maria_clone_interfaces(handlerton *aria_hton)
{
#ifndef EMBEDDED_LIBRARY
  auto &interface= aria_hton->clone_interface;
  interface.clone_capability= clone_get_capability;

  interface.clone_begin= clone_begin;
  interface.clone_copy= clone_copy;
  interface.clone_ack= clone_ack;
  interface.clone_end= clone_end;

  interface.clone_apply_begin= clone_apply_begin;
  interface.clone_apply= clone_apply;
  interface.clone_apply_end= clone_apply_end;
#endif /* EMBEDDED_LIBRARY */
}
