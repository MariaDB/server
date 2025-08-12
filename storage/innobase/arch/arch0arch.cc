/*****************************************************************************

Copyright (c) 2017, 2024, Oracle and/or its affiliates.

This program is free software; you can redistribute it and/or modify it under
the terms of the GNU General Public License, version 2.0, as published by the
Free Software Foundation.

This program is designed to work with certain software (including
but not limited to OpenSSL) that is licensed under separate terms,
as designated in a particular file or component or in included license
documentation.  The authors of MySQL hereby grant you an additional
permission to link the program and your derivative works with the
separately licensed software that they have either included with
the program or referenced in the documentation.

This program is distributed in the hope that it will be useful, but WITHOUT
ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
FOR A PARTICULAR PURPOSE. See the GNU General Public License, version 2.0,
for more details.

You should have received a copy of the GNU General Public License along with
this program; if not, write to the Free Software Foundation, Inc.,
51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA

*****************************************************************************/

/** @file arch/arch0arch.cc
 Common implementation for redo log and dirty page archiver system

 *******************************************************/

#include "arch0arch.h"
#include "arch0page.h"
#include "srv0start.h"
#include "log.h"

/** PFS thread key for log archiver background. */
mysql_pfs_key_t archiver_thread_key;

/** Archiver system global */
Arch_Sys *arch_sys= nullptr;

dberr_t Arch_Sys::init()
{
  if (!arch_sys)
    arch_sys= UT_NEW(Arch_Sys(), mem_key_archive);

  return arch_sys ? DB_SUCCESS : DB_OUT_OF_MEMORY;
}

void Arch_Sys::stop()
{
  /* To be called during shutown last phase. */
  ut_ad(srv_shutdown_state.load() >= SRV_SHUTDOWN_LAST_PHASE);
  std::chrono::milliseconds sleep_time{1};

  /* Start with 1ms and back off till 1 sec. */
  int sleep_count=0, backoff_count=0;

  while (arch_sys && arch_sys->signal_archiver())
  {
    std::this_thread::sleep_for(sleep_time);
    if (++sleep_count == 10 && backoff_count < 3)
    {
        sleep_time*= 10;
        sleep_count= 0;
        ++backoff_count;
        continue;
    }
    if (sleep_count == 30 )
      ib::warn() << "Archiver still running: Waited 30 seconds.";

    else if (sleep_count >= 600)
      ib::fatal() << "Archiver still running: Waited for 10 minutes.";
  }
}

void Arch_Sys::free()
{
  if (arch_sys)
  {
    UT_DELETE(arch_sys);
    arch_sys= nullptr;
  }
}

int Arch_Sys::start_archiver()
{
  if (!os_file_create_directory(ARCH_DIR, false))
  {
    my_error(ER_CANT_CREATE_FILE, MYF(0), ARCH_DIR, errno);
    return ER_CANT_CREATE_FILE;
  }
  int err=0;

  mysql_mutex_lock(&m_mutex);
  if (!m_archiver_active)
  {
    try
    {
      std::thread(Arch_Sys::archiver).detach();
      m_archiver_active= true;
    }
    catch (...)
    {
      my_error(ER_CANT_CREATE_THREAD, MYF(0), errno);
      m_archiver_active= false;
      err= ER_CANT_CREATE_THREAD;
    }
  }
  mysql_mutex_unlock(&m_mutex);
  return err;
}

bool Arch_Sys::signal_archiver()
{
  bool alive= false;
  mysql_mutex_lock(&m_mutex);
  if (m_archiver_active)
  {
    mysql_cond_signal(&m_cond);
    m_signalled= true;
    alive= true;
  }
  mysql_mutex_unlock(&m_mutex);
  return alive;
}

void Arch_Sys::archiver_wait()
{
  mysql_mutex_lock(&m_mutex);
  ut_ad(m_archiver_active);

  struct timespec wait_time;
  while(!m_signalled)
  {
    set_timespec(wait_time, 1);
    mysql_cond_timedwait(&m_cond, &m_mutex, &wait_time);
  }
  m_signalled= false;
  mysql_mutex_unlock(&m_mutex);
}

void Arch_Sys::archiver_stopped()
{
  mysql_mutex_lock(&m_mutex);
  m_archiver_active= false;
  mysql_mutex_unlock(&m_mutex);
}

void Arch_Sys::remove_file(const char *file_path, const char *file_name)
{
  char path[MAX_ARCH_PAGE_FILE_NAME_LEN];

  static_assert(MAX_ARCH_LOG_FILE_NAME_LEN <= MAX_ARCH_PAGE_FILE_NAME_LEN);
  ut_ad(strlen(file_path) + 1 + strlen(file_name) <
        MAX_ARCH_PAGE_FILE_NAME_LEN);

  /* Remove only LOG and PAGE archival files. */
  if (0 != strncmp(file_name, ARCH_LOG_FILE, strlen(ARCH_LOG_FILE)) &&
      0 != strncmp(file_name, ARCH_PAGE_FILE, strlen(ARCH_PAGE_FILE)) &&
      0 != strncmp(file_name, ARCH_PAGE_GROUP_DURABLE_FILE_NAME,
                   strlen(ARCH_PAGE_GROUP_DURABLE_FILE_NAME)))
    return;

  snprintf(path, sizeof(path), "%s%c%s", file_path, OS_PATH_SEPARATOR,
           file_name);

#ifdef UNIV_DEBUG
  os_file_type_t type;
  bool exists;

  os_file_status(path, &exists, &type);
  ut_ad(exists);
  ut_ad(type == OS_FILE_TYPE_FILE);
#endif /* UNIV_DEBUG */

  os_file_delete(innodb_arch_file_key, path);
}

void Arch_Sys::remove_dir(const char *dir_path, const char *dir_name)
{
  char path[MAX_ARCH_DIR_NAME_LEN];

  static_assert(sizeof(ARCH_LOG_DIR) <= sizeof(ARCH_PAGE_DIR));
  ut_ad(strlen(dir_path) + 1 + strlen(dir_name) + 1 < sizeof(path));

  /* Remove only LOG and PAGE archival directories. */
  if (0 != strncmp(dir_name, ARCH_LOG_DIR, strlen(ARCH_LOG_DIR)) &&
      0 != strncmp(dir_name, ARCH_PAGE_DIR, strlen(ARCH_PAGE_DIR)))
    return;

  snprintf(path, sizeof(path), "%s%c%s", dir_path, OS_PATH_SEPARATOR, dir_name);

#ifdef UNIV_DEBUG
  os_file_type_t type;
  bool exists;

  os_file_status(path, &exists, &type);
  ut_ad(exists);
  ut_ad(type == OS_FILE_TYPE_DIR);
#endif /* UNIV_DEBUG */

  os_file_scan_directory(path, Arch_Sys::remove_file, true);
}

/** Initialize Page and Log archiver system. */
Arch_Sys::Arch_Sys()
{
  mysql_mutex_init(0, &m_mutex, nullptr);
  mysql_cond_init(0, &m_cond, nullptr);

  m_signalled= false;
  m_archiver_active= false;

  if (srv_read_only_mode)
    m_page_sys.set_read_only_mode();
  else
    m_page_sys.recover();
}

/** Free Page and Log archiver system */
Arch_Sys::~Arch_Sys()
{
  mysql_cond_destroy(&m_cond);
  mysql_mutex_destroy(&m_mutex);
}

dberr_t Arch_Group::write_to_file(Arch_File_Ctx *from_file, byte *from_buffer,
                                  uint length, bool partial_write,
                                  bool do_persist)
{
  dberr_t err= DB_SUCCESS;
  uint write_size;

  if (m_file_ctx.is_closed())
  {
    /* First file in the archive group. */
    ut_ad(m_file_ctx.get_count() == 0);
    DBUG_EXECUTE_IF("crash_before_archive_file_creation", DBUG_SUICIDE(););

    err= m_file_ctx.open_new(m_begin_lsn, m_file_size, m_header_len);
    if (err != DB_SUCCESS)
      return err;
  }

  auto len_left= m_file_ctx.bytes_left();

  /* New file is immediately opened when current file is over. */
  ut_ad(len_left != 0);

  while (length > 0)
  {
    auto len_copy= static_cast<uint64_t>(length);

    /* Write as much as possible in current file. */
    if (len_left < len_copy)
    {
      ut_ad(len_left <= std::numeric_limits<uint>::max());
      write_size= static_cast<uint>(len_left);
    }
    else
      write_size= length;

    if (do_persist)
    {
      Arch_Page_Dblwr_Offset dblwr_offset=
          (partial_write ? ARCH_PAGE_DBLWR_PARTIAL_FLUSH_PAGE
                         : ARCH_PAGE_DBLWR_FULL_FLUSH_PAGE);

      Arch_Group::write_to_doublewrite_file(from_file, from_buffer, write_size,
                                            dblwr_offset);
    }

    if (partial_write)
    {
      DBUG_EXECUTE_IF("crash_after_partial_block_dblwr_flush", DBUG_SUICIDE(););
      err= m_file_ctx.write(from_file, from_buffer,
                            static_cast<uint>(m_file_ctx.get_offset()),
                            write_size);
    }
    else
    {
      DBUG_EXECUTE_IF("crash_after_full_block_dblwr_flush", DBUG_SUICIDE(););
      err= m_file_ctx.write(from_file, from_buffer, write_size);
    }
    if (err != DB_SUCCESS)
      return (err);

    if (do_persist)
      /* Flush the file to make sure the changes are made persistent as there
      would be no way to recover the data otherwise in case of a crash. */
      m_file_ctx.flush();

    ut_ad(length >= write_size);
    length-= write_size;

    len_left= m_file_ctx.bytes_left();

    /* Current file is over, switch to next file. */
    if (len_left == 0)
    {
      m_file_ctx.close();

      err= m_file_ctx.open_new(m_begin_lsn, m_file_size, m_header_len);
      if (err != DB_SUCCESS)
        return (err);

      DBUG_EXECUTE_IF("crash_after_archive_file_creation", DBUG_SUICIDE(););

      len_left= m_file_ctx.bytes_left();
    }
  }
  return DB_SUCCESS;
}

bool Arch_File_Ctx::delete_file(uint file_index, lsn_t begin_lsn)
{
  bool success;
  char file_name[MAX_ARCH_PAGE_FILE_NAME_LEN];

  build_name(file_index, begin_lsn, file_name, MAX_ARCH_PAGE_FILE_NAME_LEN);

  os_file_type_t type;
  bool exists;

  success= os_file_status(file_name, &exists, &type);
  if (!success || !exists)
    return (false);

  ut_ad(type == OS_FILE_TYPE_FILE);

  success= os_file_delete(innodb_arch_file_key, file_name);
  return success;
}

void Arch_File_Ctx::delete_files(lsn_t begin_lsn)
{
  bool exists;
  os_file_type_t type;
  char dir_name[MAX_ARCH_DIR_NAME_LEN];

  build_dir_name(begin_lsn, dir_name, MAX_ARCH_DIR_NAME_LEN);
  os_file_status(dir_name, &exists, &type);

  if (exists)
  {
    ut_ad(type == OS_FILE_TYPE_DIR);
    os_file_scan_directory(dir_name, Arch_Sys::remove_file, true);
  }
}

dberr_t Arch_File_Ctx::init(const char *path, const char *base_dir,
                            const char *base_file, uint num_files)
{
  m_base_len= static_cast<uint>(strlen(path));

  m_name_len=
      m_base_len + static_cast<uint>(strlen(base_file)) + MAX_LSN_DECIMAL_DIGIT;

  if (base_dir != nullptr)
  {
    m_name_len += static_cast<uint>(strlen(base_dir));
    m_name_len += MAX_LSN_DECIMAL_DIGIT;
  }

  /* Add some extra buffer. */
  m_name_len+= MAX_LSN_DECIMAL_DIGIT;

  /* In case of reinitialise. */
  if (m_name_buf != nullptr)
  {
    ut_free(m_name_buf);
    m_name_buf = nullptr;
  }
  m_name_buf= static_cast<char *>(ut_malloc(m_name_len, mem_key_archive));

  if (m_name_buf == nullptr)
    return DB_OUT_OF_MEMORY;

  m_path_name= path;
  m_dir_name= base_dir;
  m_file_name= base_file;

  strcpy(m_name_buf, path);

  if (m_name_buf[m_base_len - 1] != OS_PATH_SEPARATOR)
  {
    m_name_buf[m_base_len] = OS_PATH_SEPARATOR;
    ++m_base_len;
    m_name_buf[m_base_len]= '\0';
  }

  m_file.m_file= OS_FILE_CLOSED;
  m_index= 0;
  m_count= num_files;
  m_offset= 0;

  m_reset.clear();
  m_stop_points.clear();

  return DB_SUCCESS;
}

dberr_t Arch_File_Ctx::open(bool read_only, lsn_t start_lsn, uint file_index,
                            uint64_t file_offset, uint64_t file_size)
{
  /* Close current file, if open. */
  close();
  m_index= file_index;
  m_offset= file_offset;

  build_name(m_index, start_lsn, nullptr, 0);

  bool exists;
  os_file_type_t type;

  bool success= os_file_status(m_name_buf, &exists, &type);

  if (!success)
    return DB_CANNOT_OPEN_FILE;

  os_file_create_t option;

  if (read_only)
  {
    if (!exists)
      return DB_CANNOT_OPEN_FILE;
    option= OS_FILE_OPEN;
  }
  else
    option= exists ? OS_FILE_OPEN : OS_FILE_CREATE;

  if (option == OS_FILE_CREATE)
    /* In case of a failure, we would use the error from os_file_create. */
    std::ignore= os_file_create_subdirs_if_needed(m_name_buf);

  m_file= os_file_create(innodb_arch_file_key, m_name_buf, option,
      OS_CLONE_LOG_FILE, read_only, &success);

  if (!success)
    return DB_CANNOT_OPEN_FILE;

  if (success)
    success= os_file_seek(m_name_buf, m_file.m_file, file_offset);

  m_size= file_size;
  ut_ad(m_offset <= m_size);

  if (success)
    return DB_SUCCESS;

  close();
  return DB_IO_ERROR;
}

dberr_t Arch_File_Ctx::open_new(lsn_t start_lsn, uint64_t new_file_size,
                                uint64_t initial_file_size)
{
  auto err= open(false, start_lsn, m_count, initial_file_size, new_file_size);
  if (err != DB_SUCCESS)
    return err;
  ++m_count;
  return DB_SUCCESS;
}

dberr_t Arch_File_Ctx::open_next(lsn_t start_lsn, uint64_t file_offset,
                                 uint64_t file_size)
{
  m_index++;
  /* Reopen the same file */
  if (m_index == m_count) m_index= 0;

  /* Open next file. */
  auto error= open(true, start_lsn, m_index, file_offset, file_size);
  return error;
}

dberr_t Arch_File_Ctx::read(byte *to_buffer, uint64_t offset, uint size)
{
  ut_ad(offset + size <= m_size);
  ut_ad(!is_closed());

  auto err= os_file_read(IORequestRead, m_file, to_buffer, offset, size,
                         nullptr);
  return err;
}

dberr_t Arch_File_Ctx::resize_and_overwrite_with_zeros(uint64_t file_size)
{
  ut_ad(m_size <= file_size);
  m_size= file_size;
  byte *buf= static_cast<byte *>(ut_zalloc(file_size, mem_key_archive));

  /* Make sure that the physical file size is the same as logical by filling
  the file with all-zeroes. Page archiver recovery expects that the physical
  file size is the same as logical file size. */
  const dberr_t err= write(nullptr, buf, 0, (uint)file_size);

  ut_free(buf);

  if (err != DB_SUCCESS)
    return err;

  flush();
  return DB_SUCCESS;
}

dberr_t Arch_File_Ctx::write(Arch_File_Ctx *from_file, byte *from_buffer,
                             uint size)
{
  dberr_t err;

  if (from_buffer == nullptr)
  {
    /* write from File */
    err= os_file_copy(from_file->m_file, from_file->m_offset, m_file, m_offset,
                      size);

    if (err == DB_SUCCESS)
    {
      from_file->m_offset+= size;
      ut_ad(from_file->m_offset <= from_file->m_size);
    }

  }
  else
    /* write from buffer */
    err= os_file_write(IORequestWrite, "Track file", m_file, from_buffer,
                       m_offset, size);
  if (err != DB_SUCCESS)
    return (err);

  m_offset+= size;
  ut_ad(m_offset <= m_size);

  return DB_SUCCESS;
}

void Arch_File_Ctx::build_name(uint idx, lsn_t dir_lsn, char *buffer,
                               uint length)
{
  char *buf_ptr;
  uint buf_len;

  /* If user has passed NULL, use pre-allocated buffer. */
  if (buffer == nullptr)
  {
    buf_ptr= m_name_buf;
    buf_len= m_name_len;
  }
  else
  {
    buf_ptr= buffer;
    buf_len= length;
    strncpy(buf_ptr, m_name_buf, buf_len);
  }

  buf_ptr+= m_base_len;
  buf_len-= m_base_len;

  if (m_dir_name == nullptr)
    snprintf(buf_ptr, buf_len, "%s%u", m_file_name, idx);

  else if (dir_lsn == LSN_MAX)
    snprintf(buf_ptr, buf_len, "%s%c%s%u", m_dir_name, OS_PATH_SEPARATOR,
             m_file_name, idx);

  else
    snprintf(buf_ptr, buf_len, "%s" UINT64PF "%c%s%u", m_dir_name, dir_lsn,
             OS_PATH_SEPARATOR, m_file_name, idx);
}

void Arch_File_Ctx::build_dir_name(lsn_t dir_lsn, char *buffer, uint length)
{
  ut_ad(buffer != nullptr);

  if (m_dir_name != nullptr)
    snprintf(buffer, length, "%s%c%s" UINT64PF, m_path_name, OS_PATH_SEPARATOR,
             m_dir_name, dir_lsn);
  else
    snprintf(buffer, length, "%s", m_path_name);
}

/** Archiver background thread */
void Arch_Sys::archiver()
{
  my_thread_init();
  my_thread_set_name("ib_archiver");

  Arch_File_Ctx log_file_ctx;
  lsn_t log_arch_lsn= LSN_MAX;

  bool log_abort= false;
  bool page_abort= false;
  bool log_init= true;

  Arch_Group::init_dblwr_file_ctx(
      ARCH_DBLWR_DIR, ARCH_DBLWR_FILE, ARCH_DBLWR_NUM_FILES,
      static_cast<uint64_t>(ARCH_PAGE_BLK_SIZE) * ARCH_DBLWR_FILE_CAPACITY);

  while (!page_abort || !log_abort)
  {
    /* Archive available redo log data. */
    bool log_wait= false;
    if (!log_abort)
    {
      log_abort= arch_sys->log_sys()->archive(log_init, &log_file_ctx,
                                              &log_arch_lsn, &log_wait);
      log_init= false;
      if (log_abort)
        sql_print_information("Innodb: Exiting Log Archiver");
    }

    bool page_wait= false;
    if (!page_abort)
    {
      /* Archive in memory data blocks to disk. */
      page_abort= arch_sys->page_sys()->archive(&page_wait);

      if (page_abort)
        sql_print_information("Innodb: Exiting Page Archiver");
    }

    if (page_wait && log_wait)
      /* Nothing to archive. Wait until next trigger. */
      arch_sys->archiver_wait();
  }
  my_thread_end();
  arch_sys->archiver_stopped();
}
