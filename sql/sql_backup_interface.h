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

#include <mysqld_error.h>
#include <my_dir.h>
#include <string.h>

#ifdef __cplusplus
# include "span.h"
# include <string>
#endif

struct backup_target;
struct backup_sink;

/** A payload chunk in a sparse file that is being streamed */
struct backup_chunk
{
  /** byte offset of the start of the payload, from the start of the file */
  uint64_t offset;
  /** length of the hole */
  uint64_t length;
};

#ifdef _WIN32
/** Copy entire file.
 @param src_path  path file file to copy
 @param dst_path  path of file to copy to
 @param target    backup target
 @param sink      worker context
 @return error code (non-positive)
 @retval 0      on success
 @note Wrapper for CopyFileExA, will report error using my_error  */
# ifdef __cplusplus
extern "C"
# endif
int copy_entire_file(const char *src_path,
                     const char *dst_path,
                     const struct backup_target *target,
                     const struct backup_sink *sink);

/** Copy entire file from data directory target, preserving path.
@param path    relative path of file
@param target  backup target
@param sink    worker context
@return error code
@retval 0      on success
@note   The file will be copied to the same path relative to
        target directory. Any intermediate directories must
        already exist in the target. */
# ifdef __cplusplus
extern "C"
# endif
int copy_datafile_to_target(const char *path,
                            const struct backup_target *target,
                            const struct backup_sink *sink);

struct native_file_handle;
#else

/** Copy entire file from data directory target, preserving path.
@param datadir_fd  file descriptor of source directory
@param path        relative path of file
@param target      backup target
@param sink        worker context
@return error code
@retval 0      on success
@note   The file will be copied to the same path relative to
        target directory. Any intermediate directories must
        already exist in the target. */
# ifdef __cplusplus
extern "C"
# endif
int copy_datafile_to_target(int datadir_fd,
                            const char *path,
                            const struct backup_target *target,
                            const struct backup_sink *sink);

# if defined __APPLE__
/* You should invoke fclonefileat(2) manually before attempting
copy_entire_file() or copy_file() */
#  include <sys/attr.h>
#  include <sys/clonefile.h>
#  include <copyfile.h>
/** Copy an entire file.
@param src  source file descriptor
@param dst  target to append src to
@return error code (negative)
@retval 0   on success */
inline int copy_entire_file(int src, int dst)
{
  return fcopyfile(src, dst, NULL, COPYFILE_ALL | COPYFILE_CLONE);
}
# else
#  ifdef __cplusplus
extern "C"
#  endif
/** Copy an entire file.
@param src  source file descriptor
@param dst  target to append src to
@return error code (non-positive)
@retval 0   on success */
int copy_entire_file(int src, int dst);
# endif

/** Obtain file descriptor to source directory.
@return  File descriptor or -1 on error
#note  Should be called when BACKUP SERVER is in progress.
       Call is thread-safe and may incur synchronization cost.
       Value may be stored safely for the duration of backup.
       Lifetime is managed by SQL layer. Error is reported by my_error.
*/
# ifdef __cplusplus
extern "C"
# endif
int get_datadir_fd();

/** Copy an entire file to target.
@param src_fd  source file descriptor
@param target  backup target
@param path    target file path
@param sink    worker context
@return error code (non-positive)
@retval 0   on success
@note   Any intermediate directories must already exist in the target. */
# ifdef __cplusplus
extern "C"
# endif
int copy_fd_to_target(int src_fd,
                      const struct backup_target *target,
                      const char *path,
                      const struct backup_sink *sink);

#endif

#ifdef __cplusplus
extern "C"
{
#endif
/** Copy a portion of a file.
@param src   source file descriptor
@param dst   target to append src to
@param start first offset to copy
@param end   last offset to copy (exclusive)
@return error code (non-positive)
@retval 0   on success */
int copy_file(IF_WIN(const native_file_handle&,int) src,
              IF_WIN(const native_file_handle&,int) dst,
              uint64_t start, uint64_t end);

/** Ensure a file can be copied to a subdirectory in target.
May create the subdirectory.
@param target  backup target
@param name    subdirectory name
@return error code (non-positive)
@retval 0      on success
@note   If the directory is created, the directory containing it must
        already exist: nested directory creation is not supported. */
int ensure_target_subdir(const struct backup_target *target, const char* name);

/** Append to the configuration file.
@param target   backup target directory
@param config   the configuration file snippet to append
@param size     length of the snippet
@return error code (non-positive)
@retval 0   on success */
int backup_config_append(IF_WIN(const char*, int) target,
                         const char *config, size_t size);


/** Append to the configuration file.
@param target   backup stream
@param config   the configuration file snippet to append
@param size     length of the snippet
@return error code (non-positive)
@retval 0   on success */
int backup_stream_config(IF_WIN(HANDLE, int) stream,
                         const char *config, size_t size);

/** Start streaming a file.
@param target   backup target
@param name     file name
@param mode     file access mode
@param size     physical length of the file, in bytes
@param chunks   payload chunks of a sparse file, or nullptr
@param n_chunks number of chunks; 0 unless sparse file
@return error code (non-positive)
@retval 0   on success */
int backup_stream_start(IF_WIN(HANDLE, int) stream,
                        const char *name, mode_t mode, uint64_t size,
                        const struct backup_chunk *chunks, size_t n_chunks);

/**
   Write data to a stream.
   @param stream  backup stream
   @param buf     source buffer
   @param size    length of the buffer (usually an integer multiple of 512)
   @return error code (non-positive)
   @retval 0 on success
*/
int backup_stream_write(IF_WIN(HANDLE, int) stream, const void *buf,
                        size_t size);

/**
   Append a file snippet to the stream,
   after a corresponding call to backup_stream_start().

   Note that tar uses 512-byte blocks. If end-start is not a multiple of
   512 bytes, backup_stream_write() must be invoked to zero-pad the output.
   @param src    source file
   @param stream backup stream
   @param start  first offset to copy
   @param end    last offset to copy (exclusive)
   @return error code (non-positive)
   @retval 0   on success
*/
int backup_stream_append(IF_WIN(const native_file_handle&,int) src,
                         IF_WIN(HANDLE, int) stream,
                         uint64_t start, uint64_t end);

#ifdef __linux__
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
int backup_stream_append_async(int src, int stream,
                               uint64_t start, uint64_t end);
#elif defined _WIN32
# define backup_stream_append_async backup_stream_append_plain
#else
# define backup_stream_append_async backup_stream_append
#endif

#ifdef _WIN32
int backup_stream_append_plain(HANDLE src, HANDLE stream,
                               uint64_t start, uint64_t end);
#else
# define backup_stream_append_plain backup_stream_append
#endif

#ifdef __cplusplus
} // extern "C"

 /* RAII wrapper for my_dir() */
class Dir_scan
{
public:
  Dir_scan() = default;
  bool initialize(const char* path, myf flags) noexcept
  {
    dir_info= my_dir(path, flags);
    if (!dir_info)
    {
      my_error(ER_CANT_READ_DIR, MYF(0), path, my_errno);
      return true;
    }
    return false;
  }

  ~Dir_scan() noexcept
  {
    my_dirend(dir_info);
  }

  Dir_scan(const Dir_scan&) = delete;
  Dir_scan& operator=(const Dir_scan&) = delete;

  st_::span<const fileinfo> contents() const
  {
    assert(dir_info);
    return {dir_info->dir_entry, dir_info->number_of_files};
  }

private:
  MY_DIR *dir_info {nullptr};
};

std::string make_path(const char *base_path, const char *filename) noexcept;

#endif
