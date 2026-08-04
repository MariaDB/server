/* Copyright (c) 2026, MariaDB plc

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1335  USA */

#include "my_global.h"
#include "mdl.h"
#include "mysys_err.h"
#include "sql_class.h"
#include "sql_backup.h"
#include "sql_backup_interface.h"
#include "sql_parse.h"
#include "my_atomic_wrapper.h"
#include "tpool.h"
#include "aligned.h"

#if defined __linux__ || defined __FreeBSD__
using copying_step= ssize_t(int,int,size_t,off_t*);
template<copying_step step,bool nonblocking>
static ssize_t copy(int in_fd, int out_fd, off_t offset, off_t end) noexcept
{
  for (;;)
  {
    const size_t c{size_t(std::min<off_t>(end - offset, INT_MAX >> 20 << 20))};
    ssize_t ret= step(in_fd, out_fd, c, &offset);
    if (ret < 0)
    {
      if (nonblocking && errno == EAGAIN)
        continue;
      return ret;
    }
    if (offset == end)
      return 0;
    if (!ret)
      return -1;
  }
}

# if 1 // disable to work around https://github.com/rr-debugger/rr/issues/4059
/* Copy between files in a single (type of) file system */
static inline ssize_t
copy_step(int in_fd, int out_fd, size_t count, off_t *offset) noexcept
{
  return copy_file_range(in_fd, offset, out_fd, offset, count, 0);
}
#  define cfr(src,dst,start,end) copy<copy_step,false>(src, dst, start, end)
# endif
#endif

#ifdef _WIN32
using tpool::pread;
using tpool::pwrite;
#else
# include <sys/mman.h>
/**
   Copy a file using a memory mapping.
   @tparam stream true=write to a stream, false=pwrite to a file
   @param in_fd   source file
   @param out_fd  destination
   @param o       start offset
   @param end     last offset (exclusive)
   @return error code
   @retval 0  on success
   @retval 1  if a memory mapping failed
*/
template<bool stream>
static ssize_t mmap_copy(int in_fd, int out_fd, uint64_t o, uint64_t end)
{
# if SIZEOF_SIZE_T < 8
  if (end != size_t(end))
    return 1;
# endif
  const size_t count= size_t(end - o);
  void *p= mmap(nullptr, count, PROT_READ, MAP_SHARED, in_fd, off_t(o));
  if (p == MAP_FAILED)
    return 1;
  ssize_t ret;
  size_t c{count};
  for (const char *b= static_cast<const char*>(p);; b+= ret, o+= uint64_t(ret))
  {
    const size_t size{std::min(c, size_t(INT_MAX >> 20 << 20))};
    if (stream)
      if ((ret= backup_stream_write(out_fd, b, size)))
        break;
    ret= stream ? size : pwrite(out_fd, b, size, off_t(o));
    if (ret < 0)
      break;
    c-= ret;
    if (!c)
    {
      ret= 0;
      break;
    }
    if (!ret)
    {
      ret= -1;
      break;
    }
  }
  munmap(p, c);
  return ret;
}
#endif

/**
   Copy a file using positioned reads.
   @tparam stream true=write to a stream, false=pwrite to a file
   @param in_fd   source file
   @param out_fd  destination
   @param o       start offset
   @param end     last offset (exclusive)
   @return error code (non-positive)
   @retval 0  on success
*/
template<bool stream>
static ssize_t pread_write(IF_WIN(const native_file_handle&,int) in_fd,
                           IF_WIN(const native_file_handle&,int) out_fd,
                           uint64_t o, uint64_t end)
  noexcept
{
  constexpr size_t READ_WRITE_SIZE= 65536;
  char *b= static_cast<char*>(aligned_malloc(READ_WRITE_SIZE, 4096));
  if (!b)
    return -1;
  ssize_t ret;
  for (uint64_t count{end - o};; o+= ret)
  {
    ret= pread(in_fd, b,
               ssize_t(std::min<uint64_t>(count, READ_WRITE_SIZE)), o);
    if (ret > 0)
    {
      if (!stream)
        ret= pwrite(out_fd, b, size_t(ret), o);
      else if (backup_stream_write(out_fd, b, size_t(ret)))
      {
        ret= -1;
        break;
      }
    }
    if (ret < 0)
      break;
    count-= uint64_t(ret);
    if (!count)
    {
      ret= 0;
      break;
    }
    if (!ret)
    {
      ret= -1;
      break;
    }
  }
  aligned_free(b);
  return ret;
}

#ifdef __APPLE__
/* The inline copy_entire_file() invokes fcopyfile() */
#elif defined _WIN32
/* CopyFileEx() should be used */
#else
/** Copy a file (whole content).
@param src  source file descriptor
@param dst  target to append src to
@return error code (non-positive)
@retval 0   on success */
extern "C" int copy_entire_file(int src, int dst)
{
  return copy_file(src, dst, 0, lseek(src, 0, SEEK_END));
}
#endif

/** Copy a portion of a file.
@param src   source file descriptor
@param dst   target to append src to
@param start first offset to copy
@param end   last offset to copy (exclusive)
@return error code (non-positive)
@retval 0   on success */
extern "C" int copy_file(IF_WIN(const native_file_handle&,int) src,
                         IF_WIN(const native_file_handle&,int) dst,
                         uint64_t start, uint64_t end)
{
  assert(end >= start);
  ssize_t ret;
# ifdef cfr
  if (!(ret= cfr(src, dst, off_t(start), off_t(end))))
    return int(ret);
#  ifdef __linux__
  if (errno == EOPNOTSUPP || errno == EXDEV)
#  endif
# endif
# ifdef __linux__ // starting with Linux 2.6.33, we can rely on sendfile(2)
    return (start != 0 && off_t(start) != lseek(dst, start, SEEK_SET))
      ? -1
      : backup_stream_append_async(src, dst, start, end);
# else
#  ifndef _WIN32
  if ((ret= mmap_copy<false>(src, dst, start, end)) == 1)
#  endif
    ret= pread_write<false>(src, dst, start, end);
# endif
  assert(ret <= 0);
  return int(ret);
}

/** Append to the configuration file.
@param target   backup target directory
@param config   the configuration file snippet to append
@param size     length of the snippet
@return error code (non-positive)
@retval 0   on success */
extern "C" int backup_config_append(IF_WIN(const char*, int) target,
                                    const char *config, size_t size)
{
  /* FIXME: append to a pre-created configuration file */
#ifdef _WIN32
  HANDLE dst;
  {
    std::string path{target};
    path.append("/backup.cnf");
    dst= CreateFile(path.c_str(), GENERIC_WRITE, 0,
                    my_win_file_secattr(), CREATE_NEW,
                    FILE_ATTRIBUTE_NORMAL, nullptr);
    if (dst != INVALID_HANDLE_VALUE)
    {
      BOOL ok;
      for (;;)
      {
        DWORD written;
        ok= WriteFile(dst, config, DWORD(size), &written, nullptr);
        if (ok || GetLastError() != ERROR_IO_PENDING)
          break;
        assert(written < DWORD(size));
        config+= written;
        size-= size_t(written);
      }
      if (CloseHandle(dst) & ok)
        return 0;
    }
  }
#else
  int dst= openat(target, "backup.cnf",
                  O_CREAT | O_EXCL | O_TRUNC | O_WRONLY, 0666);
  if (dst < 0)
    return dst;
  ssize_t ret;
  for (; (ret= write(dst, config, size)) >= 0; config+= ret, size -= ret)
  {
    assert(size_t(ret) <= size);
    if (!(size-= size_t(ret)))
    {
      ret= 0;
      break;
    }
  }
  if (!(close(dst) | ret))
    return 0;
#endif
  my_error(ER_CANT_CREATE_FILE, MYF(0), "backup.cnf", errno);
  return -1;
}

/** backup context */
struct backup_target_phase
{
  /** target directory or stream */
  backup_target target;
  /** current phase of backup */
  backup_phase phase;
  /** backup worker state (output stream) */
  backup_sink sink;
  /** stream object for streaming backup */
  FILE *stream;
  /** handlerton::backup_step return value in multi-threaded operation */
  int ret;
  /** engine-specific backup context */
  std::unordered_map<const handlerton*,void*> &ha_data;
};

/**
   Inform a storage engine of an upcoming backup by invoking
   handlerton::backup_start(BACKUP_PHASE_PREPARE_START) before
   acquiring any locks.
   @param thd     current session
   @param plugin  storage engine
   @return whether the operation failed
*/
static my_bool backup_preparation(THD *thd, plugin_ref plugin, void*) noexcept
{
  const auto bs= plugin_hton(plugin)->backup_start;
  return bs && bs(thd, nullptr, BACKUP_PHASE_PREPARE_START, nullptr);
}

/**
   Invoke handlerton::backup_start() on a storage engine,
   when there are no pending handlerton::backup_step() in any thread.
   @param thd     current session
   @param plugin  storage engine
   @param arg     the backup_target_phase context
   @return whether the operation failed
*/
static my_bool backup_start(THD *thd, plugin_ref plugin, void *arg) noexcept
{
  const handlerton *hton= plugin_hton(plugin);
  backup_target_phase &t{*static_cast<backup_target_phase*>(arg)};
  assert(int{t.phase} >= 0 || t.phase == BACKUP_PHASE_FINISH);
  if (hton->backup_start)
  {
    t.sink.ha_data= t.ha_data[hton];
    void *data= hton->backup_start(thd, &t.target, t.phase, &t.sink);
    if (data == reinterpret_cast<void*>(-1))
      return true;
    assert(!t.ha_data[hton] || t.ha_data[hton] == data);
    t.ha_data[hton]= data;
  }
  return false;
}

/**
   Invoke handlerton::backup_end() on a storage engine,
   when there are no pending handlerton::backup_step() in any thread.
   @param thd     current session
   @param plugin  storage engine
   @param arg     the backup_target_phase context
   @return whether the operation failed
*/
static my_bool backup_end(THD *thd, plugin_ref plugin, void *arg) noexcept
{
  const handlerton *hton= plugin_hton(plugin);
  backup_target_phase &t{*static_cast<backup_target_phase*>(arg)};
  if (hton->backup_end)
  {
    t.sink.ha_data= t.ha_data[hton];
    return hton->backup_end(thd, &t.target, t.phase, &t.sink);
  }
  return false;
}

/**
   Invoke handlerton::backup_step() on a storage engine in a thread
   that may or may not be associated with a BACKUP SERVER connection,
   between handlerton::backup_start() and handlerton::backup_end()
   of the same backup_phase.
   @param thd     the BACKUP SERVER session
   @param plugin  storage engine
   @param arg     the backup_target_phase context
   @return whether the operation failed
*/
static my_bool backup_step(THD *thd, plugin_ref plugin, void *arg) noexcept
{
  const handlerton *hton= plugin_hton(plugin);
  backup_target_phase &t{*static_cast<backup_target_phase*>(arg)};
  assert(int{t.phase} >= 0 || t.phase == BACKUP_PHASE_FINISH);
  int res= 0;
  if (hton->backup_step)
  {
    t.sink.ha_data= t.ha_data[hton];
    while ((res= hton->backup_step(thd, &t.target, t.phase, &t.sink)))
      if (res < 0)
        break;
  }
  return res != 0;
}

/** Number of background tasks executing backup_step_callback */
static Atomic_counter<int> backup_step_callback_pending{0};

/** Invoke backup_step() in a background task */
static void backup_step_callback(void *arg) noexcept
{
  backup_target_phase &t{*static_cast<backup_target_phase*>(arg)};
  assert(!t.ret);
  t.ret= plugin_foreach_with_mask(nullptr, backup_step,
                                  MYSQL_STORAGE_ENGINE_PLUGIN,
                                  PLUGIN_IS_DELETED|PLUGIN_IS_READY, &t);
#ifndef NDEBUG
  auto was_pending=
#endif
    backup_step_callback_pending--;
  assert(was_pending);
}

/**
   Execute all handlerton::backup_step() until completion or failure.
   @param thd           current connection
   @param target_phase  backup target and phase
   @param threads       number of execution threads
   @param tp            thread pool
*/
static bool backup_steps(THD *thd, backup_target_phase *target_phase,
                         int threads, tpool::thread_pool *tp)
{
  assert(!backup_step_callback_pending);
  if (threads == 1)
    return plugin_foreach_with_mask(thd, backup_step,
                                    MYSQL_STORAGE_ENGINE_PLUGIN,
                                    PLUGIN_IS_DELETED|PLUGIN_IS_READY,
                                    target_phase);
  tpool::task *const tasks=
    static_cast<tpool::task*>(alloca(threads * sizeof *tasks));
  backup_step_callback_pending= threads - 1;
  for (int n{threads}; --n; )
  {
    target_phase[n].phase= target_phase->phase;
    tp->submit_task(new (&tasks[n]) tpool::task{backup_step_callback,
                                                &target_phase[n]});
  }
  bool fail= plugin_foreach_with_mask(thd, backup_step,
                                      MYSQL_STORAGE_ENGINE_PLUGIN,
                                      PLUGIN_IS_DELETED|PLUGIN_IS_READY,
                                      target_phase);
  while (backup_step_callback_pending)
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

  if (fail)
    return fail;

  for (int n{threads}; --n; )
    if (target_phase[n].ret)
    {
      my_error(ER_UNKNOWN_ERROR, MYF(0));
      return true;
    }

  return false;
}

bool Sql_cmd_backup::execute(THD *thd)
{
  assert(!!target == !command);

  if (check_global_access(thd, RELOAD_ACL) ||
      check_global_access(thd, SELECT_ACL) ||
      (target && error_if_data_home_dir(target, "BACKUP SERVER TO")))
    return true;

  if (thd->current_backup_stage != BACKUP_FINISHED)
  {
    my_error(ER_BACKUP_LOCK_IS_ACTIVE, MYF(0));
    return true;
  }

  bool fail{plugin_foreach_with_mask(thd, backup_preparation,
                                     MYSQL_STORAGE_ENGINE_PLUGIN,
                                     PLUGIN_IS_DELETED|PLUGIN_IS_READY,
                                     nullptr)};
  if (fail)
  {
    my_error(ER_OUT_OF_RESOURCES, MYF(0));
    return true;
  }

  /* Block concurrent BACKUP SERVER and BACKUP STAGE */
  MDL_request mdl_request;
  MDL_REQUEST_INIT(&mdl_request, MDL_key::BACKUP, "", "", MDL_BACKUP_START,
                   MDL_EXPLICIT);

  if (thd->mdl_context.acquire_lock(&mdl_request,
                                    thd->variables.lock_wait_timeout))
    return true;

  tpool::thread_pool *tp= nullptr;
  std::unordered_map<const handlerton*,void*> ha_data{};
  backup_target_phase *target_phase= static_cast<backup_target_phase*>
    (alloca(threads * sizeof *target_phase));
  if (threads > 1 && !(tp= tpool::create_thread_pool_generic()))
  {
  oor:
    my_error(ER_OUT_OF_RESOURCES, MYF(0));
  err_exit:
    thd->mdl_context.release_lock(mdl_request.ticket);
    delete tp;
    return true;
  }

  if (command)
  {
    char cmd[1024];
    for (int t{threads}; t; )
    {
      if (snprintf(cmd, sizeof cmd, "%s %d", command, t) >= int(sizeof cmd))
        goto oor;
      FILE *f= my_popen(cmd, "w");
      if (!f)
      {
        while (t < threads)
          my_pclose(target_phase[t++].stream);
        goto oor;
      }
#ifdef _WIN32
      HANDLE sink= (HANDLE) _get_osfhandle(_fileno(f));
#else
      int sink= fileno(f);
#endif
      new (&target_phase[--t])
        backup_target_phase{backup_target{IF_WIN(nullptr, -1)},
          BACKUP_PHASE_START, backup_sink{sink, nullptr}, f, 0, ha_data};
    }
  }
  else if (my_mkdir(target, 0755, MYF(MY_WME)))
    goto err_exit;
  else
  {
#ifndef _WIN32
    const int dir{open(target, O_DIRECTORY)};
    if (dir < 0)
    {
      my_error(EE_CANT_MKDIR, MYF(ME_BELL), target, errno);
      goto err_exit;
    }
#endif
    for (int t{threads}; t; )
    {
      new (&target_phase[--t])
        backup_target_phase{backup_target{IF_WIN(target, dir)},
          BACKUP_PHASE_START,
          backup_sink{backup_sink::NO_STREAM, nullptr}, nullptr, 0, ha_data};
    }
  }

  static_assert(int{MDL_BACKUP_START} + 1 == int{MDL_BACKUP_FLUSH}, "");
  static_assert(int{MDL_BACKUP_START} + 2 == int{MDL_BACKUP_WAIT_FLUSH}, "");
  static_assert(int{MDL_BACKUP_START} + 3 == int{MDL_BACKUP_WAIT_DDL}, "");
  static_assert(int{MDL_BACKUP_START} + 4 == int{MDL_BACKUP_WAIT_COMMIT}, "");
  static_assert(int{BACKUP_PHASE_START} + 1 ==
                int{BACKUP_PHASE_NO_BEGIN_NON_TRANS}, "");
  static_assert(int{BACKUP_PHASE_START} + 2 ==
                int{BACKUP_PHASE_NO_DML_NON_TRANS}, "");
  static_assert(int{BACKUP_PHASE_START} + 3 == int{BACKUP_PHASE_NO_DDL}, "");
  static_assert(int{BACKUP_PHASE_START} + 4 ==
                int{BACKUP_PHASE_NO_COMMIT}, "");
  int phase= int{BACKUP_PHASE_START};
  goto backup_phase_start;

  for (; phase <= int{BACKUP_PHASE_NO_COMMIT}; phase++)
  {
    assert(!fail);
    {
      const enum_mdl_type mdl=
        enum_mdl_type(phase - int{BACKUP_PHASE_START} + int{MDL_BACKUP_START});
      fail=
        thd->mdl_context.upgrade_shared_lock(mdl_request.ticket, mdl,
                                             thd->variables.lock_wait_timeout);
      if (fail)
        break;
    }
  backup_phase_start:
    target_phase->phase= backup_phase(phase);
    fail= plugin_foreach_with_mask(thd, backup_start,
                                   MYSQL_STORAGE_ENGINE_PLUGIN,
                                   PLUGIN_IS_DELETED|PLUGIN_IS_READY,
                                   target_phase);
    if (fail)
      break;
    fail= backup_steps(thd, target_phase, threads, tp) ||
      plugin_foreach_with_mask(thd, backup_end, MYSQL_STORAGE_ENGINE_PLUGIN,
                               PLUGIN_IS_DELETED|PLUGIN_IS_READY,
                               target_phase);
    if (fail)
      break;
  }

  /* The final part must not interfere with the use of the server datadir.
  Release the locks. */
  thd->mdl_context.release_lock(mdl_request.ticket);
  if (!fail)
  {
    target_phase->phase= BACKUP_PHASE_FINISH;
    fail= plugin_foreach_with_mask(thd, backup_start,
                                   MYSQL_STORAGE_ENGINE_PLUGIN,
                                   PLUGIN_IS_DELETED|PLUGIN_IS_READY,
                                   target_phase) ||
      backup_steps(thd, target_phase, threads, tp);
  }
  else
  {
    target_phase->phase= BACKUP_PHASE_ABORT;
    plugin_foreach_with_mask(thd, backup_end, MYSQL_STORAGE_ENGINE_PLUGIN,
                             PLUGIN_IS_DELETED|PLUGIN_IS_READY, target_phase);
    target_phase->phase= BACKUP_PHASE_FINISH;
  }

  fail=
    plugin_foreach_with_mask(thd, backup_end, MYSQL_STORAGE_ENGINE_PLUGIN,
                             PLUGIN_IS_DELETED|PLUGIN_IS_READY,
                             target_phase) || fail;
  delete tp;

  if (command)
    for (int t= threads; t--; )
      my_pclose(target_phase[t].stream);
#ifndef _WIN32
  else
    std::ignore= close(target_phase->target.fd);
#endif

  if (!fail)
    my_ok(thd);
  return fail;
}

/**
   Encode an octal string.
   @param start   first byte of buffer
   @param end     first byte after buffer
   @param n       number to encode
*/
static void ustar_write_octal(char *start, char *end, uint64_t n) noexcept
{
  for (*--end= '\0'; --end >= start; n>>= 3)
    *end= char('0' + (n & 7));
}

/**
   Encode a quantity in 12 bytes.
   @param start   first byte of the buffer
   @param n       number to encode
*/
static void ustar_write_dozen(char *start, uint64_t n) noexcept
{
  if (n < 1ULL << 33)
    ustar_write_octal(start, start + 12, n);
  else
  {
    const uint32_t head{my_htobe32(1U << 31)};
    n= my_htobe64(n);
    memcpy(start, &head, 4);
    memcpy(start + 4, &n, 8);
  }
}

/** Initialize a ustar block
@param buf  GNU tape archiver --format=oldgnu header block
@param name name of the block
@param mode file access mode
@param size physical size of the following block */
static void ustar_block_init(char *buf, const char *name, mode_t mode,
                             uint64_t size) noexcept
{
  strncpy(buf, name, 100);
  ustar_write_octal(buf + 100, buf + 108, uint64_t(mode));
  ustar_write_octal(buf + 108, buf + 116, 0/* POSIX uid */);
  ustar_write_octal(buf + 116, buf + 124, 0/* POSIX gid */);
  ustar_write_dozen(buf + 124, size);
  /* last modification time */
  ustar_write_octal(buf + 136, buf + 148, 0);
  memset(buf + 148, ' ', 9); /* initial block checksum and dummy type */
  memset(buf + 157, '\0', 100); /* name of linked file (unused) */
  memcpy(buf + 257, "ustar  ", 8);
  strncpy(buf + 265, "root" /* POSIX owner name */, 32);
  strncpy(buf + 297, "root" /* POSIX group name */, 512 - 297);
  /* The caller will fill in the rest and invoke ustar_block_checksum() */
}

/**
   Compute and write the POSIX tar block checksum.
   @param buf    POSIX tar block
*/
static void ustar_block_checksum(char *buf) noexcept
{
  uint16_t sum{0};
  for (int i{0}; i < 512; i++)
    sum+= uint16_t{uint8_t(buf[i])};
  ustar_write_octal(buf + 148, buf + 155, sum);
}

/**
   Write data to a stream.
   @param stream  backup stream
   @param buf     source buffer
   @param size    length of the buffer (usually an integer multiple of 512)
   @return error code (non-positive)
   @retval 0 on success
*/
extern "C" int backup_stream_write(IF_WIN(HANDLE, int) stream, const void *buf,
                                   size_t size)
{
#ifdef _WIN32
  for (DWORD sz= DWORD(size);;)
  {
    DWORD wrote;
    if (WriteFile(stream, buf, sz, &wrote, nullptr))
    {
      assert(wrote == sz);
      return 0;
    }
    else if (GetLastError() != ERROR_IO_PENDING)
      return -1;
    buf= static_cast<const char*>(buf) + wrote;
    sz-= wrote;
  }
#else
  do
  {
    ssize_t wrote= write(stream, buf, size);
    assert(wrote <= ssize_t(size));
    if (wrote < 0)
    {
      if (errno == EAGAIN)
        continue;
      return -1;
    }
    buf= static_cast<const char*>(buf) + wrote;
    size-= size_t(wrote);
  }
  while (size);
#endif
  return 0;
}

/**
   Copy a prefix of a NUL terminated string to a buffer, NUL-padded.
   @param b     output buffer
   @param s     NUL terminated string
   @param size  size of buf, in bytes
*/
static inline char *ustar_zeropad(char *b, const char *s, size_t size) noexcept
{
#if defined __GNUC__ && __GNUC__ >= 8
# pragma GCC diagnostic push
# pragma GCC diagnostic ignored "-Wstringop-truncation"
#endif
  return strncpy(b, s, size);
#if defined __GNUC__ && __GNUC__ >= 8
# pragma GCC diagnostic pop
#endif
}

/** Start streaming a file.
@param stream   backup stream
@param name     file name
@param mode     file access mode
@param size     physical length of the file, in bytes
@param chunks   payload chunks of a sparse file, or nullptr
@param n_chunks number of chunks; 0 unless sparse file
@return error code (non-positive)
@retval 0   on success */
extern "C"
int backup_stream_start(IF_WIN(HANDLE, int) stream,
                        const char *name, mode_t mode, uint64_t size,
                        const struct backup_chunk *chunks, size_t n_chunks)
{
  assert(stream != backup_sink::NO_STREAM);
  char buf[512];
  size_t s= strlen(name);
  if (s > 100)
  {
    /* Write a block that contains the full name length,
    followed by blocks that contain the full name, in
    tar --format=oldgnu */
    ustar_block_init(buf, "././@LongLink", 0644, s);
    ustar_block_checksum(buf);
    if (int err= backup_stream_write(stream, buf, sizeof buf))
      return err;
    const size_t whole{s & ~(sizeof buf)};
    if (whole)
      if (int err= backup_stream_write(stream, name, whole))
        return err;
    if (s - whole)
    {
      ustar_zeropad(buf, name + whole, sizeof buf);
      if (int err= backup_stream_write(stream, buf, sizeof buf))
        return err;
    }
  }

  ustar_block_init(buf, name, mode, size);
  if (!n_chunks)
    buf[156]= '0';
  else
  {
    buf[156]= 'S';
    char *h= &buf[386];
    if (n_chunks > 4)
      return -1; // FIXME; support more chunks
    for (size_t i= 0; i < n_chunks; i++, h+= 24)
    {
      ustar_write_dozen(h, chunks[i].offset);
      ustar_write_dozen(h + 12, chunks[i].length);
    }
    ustar_write_dozen(&buf[0x1e3], chunks[n_chunks - 1].offset +
                      chunks[n_chunks - 1].length);
  }
  ustar_block_checksum(buf);
  return backup_stream_write(stream, buf, sizeof buf);
}

/** Append to the configuration file.
@param target   backup stream
@param config   the configuration file snippet to append
@param size     length of the snippet
@return error code (non-positive)
@retval 0   on success */
extern "C" int backup_stream_config(IF_WIN(HANDLE, int) stream,
                                    const char *config, size_t size)
{
  /* FIXME: append to a pre-created configuration file */
  if (int ret=
      backup_stream_start(stream, "backup.cnf", 0644, size, nullptr, 0))
    return ret;
  char buf[512];
  const size_t whole{size & ~((sizeof buf) - 1)};
  if (whole)
    if (int err= backup_stream_write(stream, config, whole))
      return err;
  if (size == whole)
    return 0;
  ustar_zeropad(buf, config + whole, sizeof buf);
  return backup_stream_write(stream, buf, sizeof buf);
}

/**
   Append a file snippet to stream,
   after a corresponding call to backup_stream_start().

   Note that tar uses 512-byte blocks. If end-start is not a multiple of
   512 bytes, backup_stream_write() must be invoked to zero-pad the output.
   @param src      source file
   @param stream   backup stream
   @param start first offset to copy
   @param end   last offset to copy (exclusive)
   @return error code (non-positive)
   @retval 0   on success
*/
extern "C" int backup_stream_append(IF_WIN(const native_file_handle&,int) src,
                                    IF_WIN(HANDLE, int) stream,
                                    uint64_t start, uint64_t end)
{
  assert(stream != backup_sink::NO_STREAM);
  ssize_t ret;
#ifndef _WIN32
  if ((ret= mmap_copy<true>(src, stream, start, end)) == 1)
#endif
    ret= pread_write<true>(src, stream, start, end);
  return int(ret);
}

#ifdef __linux__
# include <sys/sendfile.h>
/** Copy a file to a stream or to a regular file. */
static inline ssize_t
send_step(int in_fd, int out_fd, size_t count, off_t *offset) noexcept
{
  return sendfile(out_fd, in_fd, offset, count);
}

/**
   Append an immutable snippet of a file to the stream,
   allowing Linux sendfile(2) to be invoked.

   Note that tar uses 512-byte blocks. If end-start is not a multiple of
   512 bytes, backup_stream_write() must be invoked to zero-pad the output.
   @param src    source file
   @param stream backup stream
   @param start  first offset to copy
   @param end    last offset to copy (exclusive)
   @return error code (non-positive)
   @retval 0   on success
*/
extern "C" int backup_stream_append_async(int src, int stream,
                                          uint64_t start, uint64_t end)
{
  assert(stream != backup_sink::NO_STREAM);
  return int(copy<send_step,true>(src, stream, off_t(start), off_t(end)));
}
#endif

#ifdef _WIN32
extern "C" int backup_stream_append_plain(HANDLE src, HANDLE stream,
                                          uint64_t start, uint64_t end)
{
  return backup_stream_append(src, stream, start, end);
}
#endif
