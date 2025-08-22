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
@file clone/src/clone_se.cc
Clone Plugin: Common SE data clone
Part of the implementation is taken from extra/mariabackup/common_engine.cc
*/

#include "handler.h"
#include "clone_handler.h"
#include "mysqld_error.h"

#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <queue>
#include <unordered_set>
#include <vector>
#include <array>
#include "mysqld.h"

extern "C" PSI_file_key get_key_file_frm();

namespace common_engine
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
  assert(serial_length == S_MAX_LENGTH);
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
  return std::make_pair(&m_serial[0], S_MAX_LENGTH);
}

bool Locator::operator==(const Locator& other) const
{
  if (m_clone_id != other.m_clone_id)
    return false;
  assert(m_version == other.m_version);
  assert(m_index == other.m_index);
  return (m_version == other.m_version && m_index == other.m_index);
}

class Descriptor
{
 public:
  Descriptor(const unsigned char *serial, size_t serial_length);
  Descriptor(const std::string &file_name, uint64_t offset);

  std::pair<std::string, uint64_t> get_file_info() const;
  std::pair<const unsigned char *, uint32_t> get_descriptor() const;

  static constexpr size_t S_MAX_META_LENGTH= 12;
  static constexpr size_t S_MAX_LENGTH= S_MAX_META_LENGTH + 2 * FN_REFLEN + 1;
  static constexpr uint64_t S_MAX_OFFSET= std::numeric_limits<uint64_t>::max();
  static constexpr uint64_t S_OFFSET_NO_DATA= S_MAX_OFFSET - 1;

 private:
  uint64_t m_file_offset= 0;
  size_t m_file_name_len= 0;
  unsigned char m_serial[S_MAX_LENGTH];
};

Descriptor::Descriptor(const unsigned char *serial, size_t serial_length)
{
  assert(serial_length <= S_MAX_LENGTH);
  memset(&m_serial[0], 0, S_MAX_LENGTH);
  auto cp_length= std::min<size_t>(serial_length, S_MAX_LENGTH);
  memcpy(&m_serial[0], serial, cp_length);

  unsigned char *ptr= &m_serial[0];
  m_file_offset= uint8korr(ptr);
  ptr+= 8;
  m_file_name_len= uint4korr(ptr);
}

Descriptor::Descriptor(const std::string &file_name, uint64_t offset)
{
  m_file_offset= offset;
  m_file_name_len= file_name.length();
  unsigned char *ptr= &m_serial[0];
  memset(ptr, 0, S_MAX_LENGTH);

  int8store(ptr, offset);
  ptr+= 8;

  int4store(ptr, static_cast<uint32_t>(m_file_name_len));
  ptr+= 4;

  if (m_file_name_len)
  {
    auto available_length= S_MAX_LENGTH - S_MAX_META_LENGTH;
    auto cp_length= std::min<uint32_t>(m_file_name_len, available_length);
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
                     const std::string &file_name)
{
  Descriptor data_desc(file_name, offset);
  auto [desc, desc_len]= data_desc.get_descriptor();
  cbk_ctx->set_data_desc(desc, desc_len);
  cbk_ctx->clear_flags();
  cbk_ctx->set_os_buffer_cache();
  return cbk_ctx->buffer_cbk(const_cast<unsigned char*>(data), data_len);
}

static int send_file(File file_desc, uchar *buf, size_t buf_size,
                     Ha_clone_cbk *cbk_ctx, const std::string &fname,
                     const std::string &tname, size_t &copied_size)
{
  assert(file_desc >= 0);
  assert(buf_size > 0);
  if (file_desc < 0 || !cbk_ctx || !buf || buf_size == 0)
  {
    my_error(ER_INTERNAL_ERROR, MYF(ME_ERROR_LOG),
             "Common SE: Clone send file invalid data");
    return ER_INTERNAL_ERROR;
  }

  int err= 0;
  bool send_file_name= true;
  copied_size= 0;

  while (size_t bytes_read= my_read(file_desc, buf, buf_size, MY_WME))
  {
    if (bytes_read == size_t(-1))
    {
      my_printf_error(ER_IO_READ_ERROR, "Error: file %s read for table %s",
          ME_ERROR_LOG, fname.c_str(), tname.c_str());
      return ER_IO_READ_ERROR;
    }
    err= send_data(cbk_ctx, buf, bytes_read, Descriptor::S_MAX_OFFSET,
                   send_file_name ? fname : "");
    if (err) break;
    copied_size+= bytes_read;
    send_file_name= false;
  }
  if (!err && copied_size == 0)
    err= send_data(cbk_ctx, buf, 0, Descriptor::S_OFFSET_NO_DATA, fname);
  return err;
}

class Table
{
 public:
  Table(std::string &db, std::string &table, std::string &fs_name) :
        m_db(std::move(db)), m_table(std::move(table)),
        m_fs_name(std::move(fs_name)) {}
  virtual ~Table() {}

  void add_file_name(const char *file_name) { m_fnames.push_back(file_name); }
  virtual int copy(THD *thd, Ha_clone_cbk *cbk_ctx, bool no_lock,
                   bool finalize);

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

int Table::copy(THD *thd, Ha_clone_cbk *cbk_ctx, bool no_lock, bool)
{
  static const size_t buf_size = 10 * 1024 * 1024;
  std::unique_ptr<uchar[]> buf;
  std::vector<File> files;
  File frm_file= -1;

  int result= 0;
  bool locked= false;

  std::string full_tname("`");
  full_tname.append(m_db).append("`.`").append(m_table).append("`");

  if (!no_lock && clone_backup_lock(thd, m_db.c_str(), m_table.c_str()))
  {
    my_printf_error(ER_INTERNAL_ERROR,
        "Error on executing BACKUP LOCK for table %s", ME_ERROR_LOG,
        full_tname.c_str());
    result= ER_INTERNAL_ERROR;
    goto exit;
  }
  else
    locked= !no_lock;

  frm_file= mysql_file_open(get_key_file_frm(), (m_fs_name + ".frm").c_str(),
                            O_RDONLY | O_SHARE, MYF(0));

  if (frm_file < 0 && !m_fnames.empty() &&
      !clone_common::ends_with(m_fnames[0].c_str(), ".ARZ") &&
      !clone_common::ends_with(m_fnames[0].c_str(), ".ARM"))
  {
    // Don't treat it as error, as the table can be dropped after it
    // was added to queue for copying
    goto exit;
  }

  for (const auto &fname : m_fnames)
  {
    File file= mysql_file_open(0, fname.c_str(),O_RDONLY | O_SHARE, MYF(0));
    if (file < 0)
    {
      my_printf_error(ER_CANT_OPEN_FILE,
          "Error on file %s open during %s table copy", ME_ERROR_LOG,
          fname.c_str(), full_tname.c_str());
      result= ER_CANT_OPEN_FILE;
      goto exit;
    }
    files.push_back(file);
  }

  if (locked && clone_backup_unlock(thd))
  {
    my_printf_error(ER_INTERNAL_ERROR,
        "Error on executing BACKUP UNLOCK for table %s", ME_ERROR_LOG,
        full_tname.c_str());
    locked= false;
    result= ER_INTERNAL_ERROR;
    goto exit;
  }
  locked= false;
  buf.reset(new uchar[buf_size]);

  for (size_t i = 0; i < m_fnames.size(); ++i)
  {
    size_t copied_size= 0;
    MY_STAT stat_info;
    if (my_fstat(files[i], &stat_info, MYF(0)))
    {
      my_printf_error(ER_INTERNAL_ERROR,
          "Error: failed to get stat info for file %s of table %s",
          ME_ERROR_LOG, m_fnames[i].c_str(), full_tname.c_str());
      goto exit;
    }
    result= send_file(files[i], buf.get(), buf_size, cbk_ctx, m_fnames[i],
                      full_tname, copied_size);
    if (result)
      goto exit;

    mysql_file_close(files[i], MYF(0));
    files[i] = -1;
    my_printf_error(ER_CLONE_SERVER_TRACE,
        "Common SE: Copied file %s for table %s, %zu bytes",
        MYF(ME_NOTE | ME_ERROR_LOG_ONLY), m_fnames[i].c_str(),
        full_tname.c_str(), copied_size);
  }
exit:
  if (frm_file >= 0)
  {
    m_version= clone_common::read_table_version_id(frm_file);
    mysql_file_close(frm_file, MYF(0));
  }

  if (locked && clone_backup_unlock(thd))
  {
    my_printf_error(ER_INTERNAL_ERROR, "Error on BACKUP UNLOCK for table %s",
                    ME_ERROR_LOG, full_tname.c_str());
  }

  for (auto file : files)
    if (file >= 0) mysql_file_close(file, MYF(0));
  return result;
}

// Append-only tables
class Log_Table : public Table
{
 public:
  Log_Table(std::string &db, std::string &table, std::string &fs_name) :
            Table(db, table, fs_name) {}

  virtual ~Log_Table() { (void)close(); }

  int copy(THD *thd, Ha_clone_cbk *cbk_ctx, bool no_lock, bool finalize) override;
  int close();

 private:
  int open();

 private:
  std::vector<File> m_src;
};

int Log_Table::open()
{
  assert(m_src.empty());

  std::string full_tname("`");
  full_tname.append(m_db).append("`.`").append(m_table).append("`");

  for (const auto &fname : m_fnames)
  {
    File file= mysql_file_open(0, fname.c_str(),O_RDONLY | O_SHARE, MYF(0));
    if (file < 0)
    {
      my_printf_error(ER_CANT_OPEN_FILE,
          "Error on file %s open during %s log table copy", ME_ERROR_LOG,
          fname.c_str(), full_tname.c_str());
      return ER_CANT_OPEN_FILE;
    }
    m_src.push_back(file);

    MY_STAT stat_info;
    if (my_fstat(file, &stat_info, MYF(0)))
    {
      my_printf_error(ER_INTERNAL_ERROR,
          "Error: failed to get stat info for file %s of log table %s",
          ME_ERROR_LOG, fname.c_str(), full_tname.c_str());
      return ER_INTERNAL_ERROR;
    }
  }

  auto frm_file= mysql_file_open(get_key_file_frm(), (m_fs_name + ".frm").c_str(),
                                 O_RDONLY | O_SHARE, MYF(0));
  if (frm_file < 0 && !m_fnames.empty() &&
      !clone_common::ends_with(m_fnames[0].c_str(), ".ARZ") &&
      !clone_common::ends_with(m_fnames[0].c_str(), ".ARM"))
  {
    my_printf_error(ER_CANT_OPEN_FILE,
        "Error: .frm file open for log table %s", ME_ERROR_LOG,
        full_tname.c_str());
    return ER_CANT_OPEN_FILE;
  }
  m_version= clone_common::read_table_version_id(frm_file);
  mysql_file_close(frm_file, MYF(0));
  return 0;
}

int Log_Table::close()
{
  while (!m_src.empty())
  {
    auto f= m_src.back();
    m_src.pop_back();
    mysql_file_close(f, MYF(0));
  }
  return 0;
}

int Log_Table::copy(THD *thd, Ha_clone_cbk *cbk_ctx, bool no_lock, bool finalize)
{
  int err= 0;
  static const size_t buf_size= 10 * 1024 * 1024;
  std::string full_tname("`");
  full_tname.append(m_db).append("`.`").append(m_table).append("`");

  auto err_exit= [&](int err)
  {
    close();
    return err;
  };

  if (m_src.empty())
  {
    err= open();
    if (err)
      return err_exit(err);
  }
  std::unique_ptr<uchar[]> buf(new uchar[buf_size]);
  for (size_t i= 0; i < m_src.size(); ++i)
  {
    // .CSM can be rewritten (see write_meta_file() usage in ha_tina.cc)
    if (!finalize && clone_common::ends_with(m_fnames[i].c_str(), ".CSM"))
      continue;
    size_t copied_size= 0;

    int err= send_file(m_src[i], buf.get(), buf_size, cbk_ctx, m_fnames[i],
                       full_tname, copied_size);
    if (err)
      return err_exit(err);

    my_printf_error(ER_CLONE_SERVER_TRACE,
        "Common SE: Copied file %s for log table %s, %zu bytes",
        MYF(ME_NOTE | ME_ERROR_LOG_ONLY), m_fnames[i].c_str(),
        full_tname.c_str(), copied_size);
  }
  return 0;
}

class Job_Repository
{
 public:
  using Job= std::function<int(THD *,Ha_clone_cbk *, uint32_t, int)>;
  void add_one(Job &&job);
  void finish(int err, Ha_clone_stage stage);
  int consume(THD *thd, uint32_t thread_id, Ha_clone_cbk *cbk,
              Ha_clone_stage stage, int err);
  Ha_clone_stage last_finished_stage();
 private:
  std::mutex m_mutex;
  std::condition_variable m_cv;
  std::queue<Job> m_jobs;
  bool m_finished[HA_CLONE_STAGE_MAX]= {false};
  int m_error= 0;
};

void Job_Repository::add_one(Job &&job)
{
  std::unique_lock<std::mutex> lock(m_mutex);
  m_jobs.push(std::forward<Job>(job));
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
      lock.lock();
    }
    if (m_error && !err)
    {
      my_error(ER_INTERNAL_ERROR, MYF(ME_ERROR_LOG),
               "Common SE: Clone error in concurrent task");
      err= m_error;
      break;
    }
    else if (err && !m_error)
    {
      m_error= err;
      break;
    }
    m_cv.wait(lock, [&]
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
  int open(const std::string &path, const std::string &file);
  void close();

  uint32_t m_task_id= 0;
  File m_file= -1;
  std::string m_cur_file;
};

int Thread_Context::open(const std::string &path, const std::string &file)
{
  /* Close previous file if there. */
  close();

  char fullpath[FN_REFLEN];
  fn_format(fullpath, file.c_str(), path.c_str(), "", MYF(MY_RELATIVE_PATH));

  size_t dirpath_len= 0;
  char dirpath[FN_REFLEN];
  dirname_part(dirpath, fullpath, &dirpath_len);

  /* Make schema directory path and create file, if needed. */
  if (my_mkdir(dirpath, 0777, MYF(0)) >= 0 || my_errno == EEXIST)
  {
    int open_flags= O_WRONLY | O_BINARY | O_APPEND;
    m_file= mysql_file_open(0, fullpath, open_flags, MYF(0));
    if (m_file < 0)
    {
      open_flags|= O_CREAT;
      m_file= mysql_file_open(0, fullpath, open_flags, MYF(0));
    }
  }
  if (m_file < 0)
  {
    m_file= -1;
    my_error(ER_CANT_OPEN_FILE, MYF(ME_ERROR_LOG), fullpath, my_errno);
    return ER_CANT_OPEN_FILE;
  }
  m_cur_file.assign(file);
  return 0;
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
      m_data_dir(datadir ? datadir : ".") {}

  void set_error(int err);
  int check_error(THD *thd);

  int clone_low(THD *thd, uint32_t task_id,
		Ha_clone_stage stage, Ha_clone_cbk *cbk);

  int clone(THD *thd, uint32_t task_id, Ha_clone_stage stage,
            Ha_clone_cbk *cbk);
  int apply(THD *thd, uint32_t task_id, Ha_clone_cbk *cbk);

  size_t attach();
  bool detach(size_t id);

  Locator &get_locator() { return m_loc; }
  static constexpr size_t S_MAX_TASKS= 128;

  bool max_task_reached() const
  {
    assert(m_next_task <= S_MAX_TASKS);
    return m_next_task >= S_MAX_TASKS;
  }

 private:
  int scan(const std::unordered_set<std::string> &exclude_tables,
           bool add_processed, bool no_lock, bool collect_log_and_stats);

  void copy_log_tables(bool finalize);
  void copy_stats_tables();

  int copy_table_job(Table *table, bool no_lock, bool delete_table,
                     bool finalize, THD *thd, Ha_clone_cbk *cbk,
		     uint32_t thread_id, int in_error);
  int copy_file_job(std::string *file_name, THD *thd, Ha_clone_cbk *cbk,
                    uint32_t thread_id, int in_error);

 private:
  bool m_is_copy= true;
  /** Number of threads attached; Protected by Clone_Sys::mutex_ */
  size_t m_num_threads= 0;
  size_t m_next_task= 0;
  int m_error= 0;

  Locator m_loc;
  std::string m_data_dir;
  std::array<Thread_Context, S_MAX_TASKS> m_thread_ctxs;

  Job_Repository m_jobs;

  std::unordered_map<table_key_t, std::unique_ptr<Log_Table>> m_log_tables;
  std::unordered_map<table_key_t, std::unique_ptr<Table>> m_stats_tables;
  std::unordered_set<table_key_t> m_processed_tables;
};

size_t Clone_Handle::attach()
{
  /* ID is the index into the m_thread_ctxs vector. */
  auto id= m_next_task++;
  assert(id < S_MAX_TASKS);

  auto &ctx= m_thread_ctxs[id];
  ctx.m_task_id= id;
  assert(ctx.m_file == -1);

  m_num_threads++;
  assert(m_thread_ctxs.size() >= m_num_threads);

  return id;
}

bool Clone_Handle::detach(size_t id)
{
  auto &ctx= m_thread_ctxs[id];
  ctx.close();
  assert(m_num_threads > 0);
  return (0 == --m_num_threads);
}

int Clone_Handle::copy_file_job(std::string *file_name, THD *thd,
                                Ha_clone_cbk *cbk,
                                uint32_t, int in_error)
{
  int err= in_error;
  if (err)
  {
    delete file_name;
    return err;
  }
  File file= mysql_file_open(0, file_name->c_str(), O_RDONLY | O_SHARE,
                             MYF(0));
  if (file < 0)
  {
    my_printf_error(ER_CANT_OPEN_FILE, "Error on opening file: %s",
                    MYF(ME_ERROR_LOG), file_name->c_str());
    err= ER_CANT_OPEN_FILE;
  }
  else
  {
    size_t copied_size= 0;
    static const size_t buf_size = 10 * 1024 * 1024;
    std::unique_ptr<uchar[]> buf= std::make_unique<uchar[]>(buf_size);

    err= send_file(file, buf.get(), buf_size, cbk, (*file_name), "",
                   copied_size);
    mysql_file_close(file, MYF(0));
  }
  delete file_name;
  return err;
}

int Clone_Handle::copy_table_job(Table *table, bool no_lock, bool delete_table,
                                 bool finalize, THD *thd,
				 Ha_clone_cbk *cbk, uint32_t,
                                 int in_error)
{
  int err= in_error ? in_error : table->copy(thd, cbk, no_lock, finalize);

  /* TODO: Post Copy Hook for DDL */
  // if (!err && m_table_post_copy_hook)
  //   m_table_post_copy_hook(table->get_db(), table->get_table(),
  //                          table->get_version());
  if (delete_table)
    delete table;

  return err;
}


int Clone_Handle::scan(const std::unordered_set<table_key_t> &exclude_tables,
                       bool add_processed, bool no_lock,
                       bool collect_log_and_stats)
{
  my_printf_error(ER_CLONE_SERVER_TRACE, "Common SE: Start scanning common"
      " engine tables, need backup locks: %d, collect log and stat tables: %d",
      MYF(ME_NOTE | ME_ERROR_LOG_ONLY), no_lock, collect_log_and_stats);
  std::unordered_map<table_key_t, std::unique_ptr<Table>> found_tables;

  std::set<std::string> ext_list=
    {".MYD", ".MYI", ".MRG", ".ARM", ".ARZ", ".CSM", ".CSV", ".MAD", ".MAI"};
  std::set<std::string> aria_list= {".MAD", ".MAI"};

  std::set<std::string> gen_list=
    {".frm", ".isl", ".TRG", ".TRN", ".opt", ".par"};
  if (!collect_log_and_stats)
  {
    std::set copy_gen= gen_list;
    ext_list.merge(copy_gen);
  }

  namespace fsys= std::filesystem;
  clone_common::foreach_file_in_dir(m_data_dir,
                                    [&](const std::filesystem::path& file_path)
  {
    std::string extn= file_path.extension().string();
    bool is_aria= (aria_list.find(extn) != aria_list.end());
    bool is_gen= (gen_list.find(extn) != gen_list.end());

    if (!collect_log_and_stats && is_aria)
      return;

    /* TODO: Partial Backup */
    // if (check_if_skip_table(file_path))
    // {
    //   my_printf_error(ER_CLONE_SERVER_TRACE, "Common SE: Skipping %s.",
    //                   MYF(ME_NOTE | ME_ERROR_LOG_ONLY), file_path);
    //   return;
    // }
    const char* fpath= nullptr;
#ifdef _WIN32
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
    auto db_table_fs=
      clone_common::convert_filepath_to_tablename(fpath);
    auto tk= table_key(std::get<0>(db_table_fs), std::get<1>(db_table_fs));

    // log and stats tables are only collected in this function,
    // so there is no need to filter out them with exclude_tables.
    if (collect_log_and_stats)
    {
      if (clone_common::is_log_table(std::get<0>(db_table_fs).c_str(),
                       std::get<1>(db_table_fs).c_str()))
      {
        auto table_it= m_log_tables.find(tk);
        if (table_it == m_log_tables.end())
        {
          my_printf_error(ER_CLONE_SERVER_TRACE,
            "Common SE: Log table found: %s", MYF(ME_NOTE | ME_ERROR_LOG_ONLY),
            tk.c_str());
          table_it= m_log_tables.emplace(tk,
              std::unique_ptr<Log_Table>(new Log_Table(std::get<0>(db_table_fs),
              std::get<1>(db_table_fs), std::get<2>(db_table_fs)))).first;
        }
        my_printf_error(ER_CLONE_SERVER_TRACE,
          "Common SE: Collect log table file: %s",
          MYF(ME_NOTE | ME_ERROR_LOG_ONLY), fpath);
        table_it->second->add_file_name(fpath);
	return;
      }
      // Aria can handle statistics tables
      else if (clone_common::is_stats_table(std::get<0>(db_table_fs).c_str(),
        std::get<1>(db_table_fs).c_str()) && !is_aria)
      {
        auto table_it = m_stats_tables.find(tk);
        if (table_it == m_stats_tables.end())
        {
          my_printf_error(ER_CLONE_SERVER_TRACE,
            "Common SE: Stats table found: %s",
            MYF(ME_NOTE | ME_ERROR_LOG_ONLY), tk.c_str());
          table_it= m_stats_tables.emplace(tk,
              std::unique_ptr<Table>(new Table(std::get<0>(db_table_fs),
              std::get<1>(db_table_fs), std::get<2>(db_table_fs)))).first;
        }
        my_printf_error(ER_CLONE_SERVER_TRACE,
          "Common SE: Collect stats table file: %s",
          MYF(ME_NOTE | ME_ERROR_LOG_ONLY), fpath);
        table_it->second->add_file_name(fpath);
        return;
      }
    }
    else if(is_gen)
    {
      auto file_name= std::make_unique<std::string>(fpath);
      using namespace std::placeholders;
      m_jobs.add_one(std::bind(&Clone_Handle::copy_file_job, this,
                               file_name.release(), _1, _2, _3, _4));
      return;
    }
    else if (clone_common::is_log_table(std::get<0>(db_table_fs).c_str(),
                                        std::get<1>(db_table_fs).c_str()) ||
             clone_common::is_stats_table(std::get<0>(db_table_fs).c_str(),
                                          std::get<1>(db_table_fs).c_str()))
      return;
    if (is_aria)
      return;

    if (exclude_tables.count(tk))
    {
      my_printf_error(ER_CLONE_SERVER_TRACE,
          "Common SE: Skip table %s as it is in exclude list",
          MYF(ME_NOTE | ME_ERROR_LOG_ONLY), tk.c_str());
      return;
    }
    auto table_it= found_tables.find(tk);
    if (table_it == found_tables.end())
    {
      table_it= found_tables.emplace(tk,
          std::unique_ptr<Table>(new Table(std::get<0>(db_table_fs),
          std::get<1>(db_table_fs), std::get<2>(db_table_fs)))).first;
    }
    table_it->second->add_file_name(fpath);
  }, ext_list);

  for (auto &table_it : found_tables)
  {
    using namespace std::placeholders;
    m_jobs.add_one(std::bind(&Clone_Handle::copy_table_job, this,
                             table_it.second.release(), no_lock,
                             true, false, _1, _2, _3, _4));
    if (add_processed)
      m_processed_tables.insert(table_it.first);
  }
  my_printf_error(ER_CLONE_SERVER_TRACE,
    "Common SE: Stop scanning common engine tables",
    MYF(ME_NOTE | ME_ERROR_LOG_ONLY));
  return 0;
}

void Clone_Handle::copy_log_tables(bool finalize)
{
  for (auto &table_it : m_log_tables)
  {
    // Do not execute BACKUP LOCK for log tables as it's supposed
    // that they must be copied on BLOCK_DDL and BLOCK_COMMIT locks.
    using namespace std::placeholders;
    if (finalize)
      /* In final state release the table objects */
      m_jobs.add_one(std::bind(&Clone_Handle::copy_table_job, this,
                     table_it.second.release(), true, true, true, _1, _2, _3, _4));
    else
      m_jobs.add_one(std::bind(&Clone_Handle::copy_table_job, this,
                     table_it.second.get(), true, false, false, _1, _2, _3, _4));
  }
  if (finalize)
    m_log_tables.clear();
}

void Clone_Handle::copy_stats_tables()
{
  for (auto &table_it : m_stats_tables)
  {
    // Do not execute BACKUP LOCK for stats tables as it's supposed
    // that they must be copied on BLOCK_DDL and BLOCK_COMMIT locks.
    // Delete stats table object after copy (see copy_table_job())
    using namespace std::placeholders;
    m_jobs.add_one(std::bind(&Clone_Handle::copy_table_job, this,
                   table_it.second.release(), true, true, false, _1, _2, _3, _4));
  }
  m_stats_tables.clear();
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
    assert(attach);
    /* Operation has finished already */
    my_error(ER_INTERNAL_ERROR, MYF(ME_ERROR_LOG),
             "Common SE: Clone add task refers non-existing clone");
    /* No active clone to attach to. */
    return ER_INTERNAL_ERROR;
  }

  if (clone_hdl->max_task_reached())
  {
    assert(attach);
    my_error(ER_INTERNAL_ERROR, MYF(ME_ERROR_LOG),
             "Common SE: Maximum Tasks reached");
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
    assert(clones[index] == clone_hdl);
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

static Clone_Sys *clone_sys;

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
  /* Currently the write is append only. */
  assert(offset == Descriptor::S_MAX_OFFSET ||
         offset == Descriptor::S_OFFSET_NO_DATA);

  int err= 0;
  if (!file_name.empty() && (err= ctx.open(m_data_dir, file_name)))
    return err;

  if (offset == Descriptor::S_OFFSET_NO_DATA)
  {
    ctx.close();
    return 0;
  }
  Ha_clone_file file;
  assert(ctx.m_file >= 0);
#ifdef _WIN32
  file.type= Ha_clone_file::FILE_HANDLE;
  file.file_handle= static_cast<void *>(my_get_osfhandle(ctx.m_file));
#else
  file.type= Ha_clone_file::FILE_DESC;
  file.file_desc= ctx.m_file;
#endif /* _WIN32 */

  cbk->set_os_buffer_cache();
  return cbk->apply_file_cbk(file);
}

int Clone_Handle::clone_low(THD *thd, uint32_t task_id,
			    Ha_clone_stage stage, Ha_clone_cbk *cbk)
{
  int err= 0;
  std::unordered_set<std::string> tables_in_use;

  switch (stage)
  {
    case HA_CLONE_STAGE_CONCURRENT:
      break;
    case HA_CLONE_STAGE_NT_DML_BLOCKED:
      if (task_id != 0)
        break;
      /* TODO: get_tables_in_use() : "SHOW OPEN TABLES WHERE In_use = 1" */
      err= scan(tables_in_use, true, false, true);
      break;
    case HA_CLONE_STAGE_DDL_BLOCKED:
      if (task_id != 0)
        break;
      err= scan(m_processed_tables, false, true, false);
      if (!err)
        copy_log_tables(false);
      break;
    case HA_CLONE_STAGE_SNAPSHOT:
      if (task_id != 0)
        break;
      copy_log_tables(true);
      copy_stats_tables();
      break;
    case HA_CLONE_STAGE_END:
      break;
    case HA_CLONE_STAGE_MAX:
     assert(false);
     err= ER_INTERNAL_ERROR;
     my_error(err, MYF(ME_ERROR_LOG), "Common SE: Invalid Execution Stage");
     break;
  }
  if (task_id == 0)
    m_jobs.finish(err, stage);
  err= m_jobs.consume(thd, task_id, cbk, stage, err);
  set_error(err);
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
} // namespace common_engine

/** Dummy SE handlerton for cloning common data and SEs that don't have clone
interfaces defined. */
struct handlerton clone_storage_engine;

static void clone_get_capability(Ha_clone_flagset &flags)
{
  flags.reset();
  flags.set(HA_CLONE_BLOCKING);
  flags.set(HA_CLONE_MULTI_TASK);
}

static int clone_begin(THD *, const uchar *&loc, uint &loc_len,
                       uint &task_id, Ha_clone_type, Ha_clone_mode mode)
{
  common_engine::Locator *in_loc= nullptr;
  if (loc)
    in_loc= new(std::nothrow) common_engine::Locator(loc, loc_len);
  int err= 0;

  const std::lock_guard<std::mutex> lock(common_engine::Clone_Sys::mutex_);
  auto clone_hdl= common_engine::clone_sys->find(in_loc, true);

  switch (mode)
  {
    case HA_CLONE_MODE_START:
      err= common_engine::clone_sys->start(true, false, clone_hdl, task_id,
                                           in_loc);
      break;
    case HA_CLONE_MODE_ADD_TASK:
      err= common_engine::clone_sys->start(true, true, clone_hdl, task_id,
                                           in_loc);
      break;
    case HA_CLONE_MODE_RESTART:
      err=ER_NOT_SUPPORTED_YET;
      my_error(ER_NOT_SUPPORTED_YET, MYF(ME_ERROR_LOG),
               "Common SE: Clone Restart after network failure");
      break;
    case HA_CLONE_MODE_VERSION:
    case HA_CLONE_MODE_MAX:
      err= ER_INTERNAL_ERROR;
      my_error(err, MYF(ME_ERROR_LOG), "Common SE: Clone Begin Invalid Mode");
      assert(false);
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
  assert(loc);
  std::unique_ptr<common_engine::Locator>
    in_loc(new(std::nothrow) common_engine::Locator(loc, loc_len));

  auto clone_hdl= common_engine::clone_sys->get(in_loc->index(), true);
  int err= clone_hdl ? clone_hdl->check_error(thd) : 0;

  if (!clone_hdl || err != 0)
    return err;

  return clone_hdl->clone(thd, task_id, stage, cbk);
}

static int clone_ack(THD *, const uchar *loc, uint loc_len,
                     uint, int in_err, Ha_clone_cbk *)
{
  DBUG_ASSERT(loc);
  std::unique_ptr<common_engine::Locator>
    in_loc(new(std::nothrow) common_engine::Locator(loc, loc_len));
  auto clone_hdl= common_engine::clone_sys->get(in_loc->index(), true);
  DBUG_ASSERT(clone_hdl);
  if (!clone_hdl)
    return 0;
  clone_hdl->set_error(in_err);
  return 0;
}

static int clone_end(THD *, const uchar *loc, uint loc_len, uint task_id,
                     int in_err)
{
  assert(loc);
  std::unique_ptr<common_engine::Locator>
    in_loc(new(std::nothrow) common_engine::Locator(loc, loc_len));
  auto clone_hdl= common_engine::clone_sys->get(in_loc->index(), true);

  assert(clone_hdl);
  clone_hdl->set_error(in_err);

  const std::lock_guard<std::mutex> lock(common_engine::Clone_Sys::mutex_);
  return common_engine::clone_sys->stop(true, clone_hdl, task_id);
}

static int clone_apply_begin(THD *, const uchar *&loc,
                             uint &loc_len, uint &task_id, Ha_clone_mode mode,
                             const char *data_dir)
{
  common_engine::Locator *in_loc= nullptr;
  if (loc)
    in_loc= new(std::nothrow) common_engine::Locator(loc, loc_len);
  int err= 0;

  const std::lock_guard<std::mutex> lock(common_engine::Clone_Sys::mutex_);
  auto clone_hdl= common_engine::clone_sys->find(in_loc, false);

  switch (mode)
  {
    case HA_CLONE_MODE_VERSION:
    case HA_CLONE_MODE_START:
      assert(!clone_hdl);
      err= common_engine::clone_sys->start(false, false, clone_hdl, task_id,
                                           in_loc, data_dir);
      task_id= 0;
      break;
    case HA_CLONE_MODE_ADD_TASK:
      err= common_engine::clone_sys->start(false, true, clone_hdl, task_id,
                                           in_loc);
      break;
    case HA_CLONE_MODE_RESTART:
      err=ER_NOT_SUPPORTED_YET;
      my_error(ER_NOT_SUPPORTED_YET, MYF(ME_ERROR_LOG),
               "Common SE: Clone Restart after network failure");
      break;
    case HA_CLONE_MODE_MAX:
      err= ER_INTERNAL_ERROR;
      my_error(err, MYF(ME_ERROR_LOG), "Common SE: Clone Begin Invalid Mode");
      assert(false);
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
  assert(loc);
  std::unique_ptr<common_engine::Locator>
    in_loc(new(std::nothrow) common_engine::Locator(loc, loc_len));

  auto clone_hdl= common_engine::clone_sys->get(in_loc->index(), false);

  assert(in_err != 0 || cbk != nullptr);
  if (clone_hdl && (in_err != 0 || cbk == nullptr))
  {
    clone_hdl->set_error(in_err);
    my_printf_error(ER_CLONE_CLIENT_TRACE, "Common SE: Set Error Code %d",
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
  assert(loc);
  std::unique_ptr<common_engine::Locator>
    in_loc(new(std::nothrow) common_engine::Locator(loc, loc_len));
  auto clone_hdl= common_engine::clone_sys->get(in_loc->index(), false);
  assert(clone_hdl);
  clone_hdl->set_error(in_err);

  const std::lock_guard<std::mutex> lock(common_engine::Clone_Sys::mutex_);
  return common_engine::clone_sys->stop(false, clone_hdl, task_id);
}

void init_clone_storage_engine()
{
  clone_storage_engine.db_type= DB_TYPE_UNKNOWN;

  auto &interface= clone_storage_engine.clone_interface;
  interface.clone_capability= clone_get_capability;

  interface.clone_begin= clone_begin;
  interface.clone_copy= clone_copy;
  interface.clone_ack= clone_ack;
  interface.clone_end= clone_end;

  interface.clone_apply_begin= clone_apply_begin;
  interface.clone_apply= clone_apply;
  interface.clone_apply_end= clone_apply_end;
  common_engine::clone_sys= new(std::nothrow) common_engine::Clone_Sys();
}

void deinit_clone_storage_engine()
{
  delete common_engine::clone_sys;
  common_engine::clone_sys= nullptr;
}
