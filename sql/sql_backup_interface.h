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

#pragma once
#include <stdint.h>

/** BACKUP SERVER target */
struct backup_target
{
#ifdef _WIN32
  /** Target directory path name, or nullptr if streaming */
  const char *path;
#else
  /** Target directory descriptor, or -1 if streaming */
  int fd;
#endif
};

/** BACKUP SERVER worker specific context */
struct backup_sink
{
#ifdef _WIN32
# ifdef __cplusplus
  /** A value indicating an invalid stream */
  static constexpr HANDLE NO_STREAM{INVALID_HANDLE_VALUE};
# endif
  /** Target pipe, or NO_STREAM if path!=nullptr */
  HANDLE stream;
#else
# ifdef __cplusplus
  /** A value indicating an invalid file descriptor or stream */
  static constexpr int NO_STREAM{-1};
# endif
  /** Target pipe, or NO_STREAM if copying to a directory */
  int stream;
#endif
  /** storage engine context returned by handlerton::backup_start() */
  void *ha_data;
};

/** BACKUP SERVER execution phase; @see Sql_cmd_backup::execute() */
enum backup_phase
{
  /** finish backup, possibly after BACKUP_PHASE_ABORT */
  BACKUP_PHASE_FINISH= -2,
  /** abort any operation */
  BACKUP_PHASE_ABORT= -1,
  /** preparatory phase executed while holding no locks */
  BACKUP_PHASE_PREPARE_START= 0,
  /** initial actual work phase; @see MDL_BACKUP_START */
  BACKUP_PHASE_START,
  /** copy while new writes to non-transactional tables are blocked;
  @see MDL_BACKUP_FLUSH */
  BACKUP_PHASE_NO_BEGIN_NON_TRANS,
  /** copy while any writes to non-transactional tables are blocked;
  @see MDL_BACKUP_WAIT_FLUSH */
  BACKUP_PHASE_NO_DML_NON_TRANS,
  /** copy files while DDL is blocked; @see MDL_BACKUP_WAIT_DDL */
  BACKUP_PHASE_NO_DDL,
  /** determine the logical time of the backup and copy any
  remaining files while MDL_BACKUP_WAIT_COMMIT is active;
  this is followed by BACKUP_PHASE_FINISH */
  BACKUP_PHASE_NO_COMMIT
};

/** A payload chunk in a sparse file that is being streamed */
struct backup_chunk
{
  /** byte offset of the start of the payload, from the start of the file */
  uint64_t offset;
  /** length of the hole */
  uint64_t length;
};

/** File descriptor */
typedef IF_WIN(HANDLE, int) backup_fd;

#ifdef _WIN32
/* Use CopyFileEx() to copy entire files */
#elif defined __APPLE__
/* You should invoke fclonefileat(2) manually before attempting
copy_entire_file() or backup::copy() */
# include <sys/attr.h>
# include <sys/clonefile.h>
# include <copyfile.h>
/** Copy an entire file.
@param src  source file descriptor
@param dst  target to append src to
@return error code (negative)
@retval 0   on success */
inline int copy_entire_file(int src, int dst)
{
  return fcopyfile(src, dst, NULL, COPYFILE_ALL | COPYFILE_CLONE);
}
#else
# ifdef __cplusplus
extern "C"
# endif
/** Copy an entire file.
@param src  source file descriptor
@param dst  target to append src to
@return error code (non-positive)
@retval 0   on success */
int copy_entire_file(int src, int dst);
#endif

#ifdef __cplusplus
# ifdef _WIN32
struct native_file_handle;
# endif
namespace backup {

typedef IF_WIN(native_file_handle, int) handle;

/**
   Copy a portion of a file.
   @param src   source file descriptor
   @param dst   target to append src to
   @param start first offset to copy
   @param end   last offset to copy (exclusive)
   @return error code (non-positive)
   @retval 0   on success
*/
int copy(handle src, backup_fd dst, uint64_t start, uint64_t end) noexcept;

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
int append(handle src, backup_fd stream, uint64_t start, uint64_t end)
  noexcept;
}
#endif

#if defined _WIN32 || defined __FreeBSD__
/* There is no special variant of backup::copy(). */
#else
# if SIZEOF_SIZE_T > 4
#  ifdef __cplusplus
extern "C"
#  endif
/**
   Copy from a memory mapping to a file.
   @param map   source file mapping
   @param dst   target to append map to
   @param start first offset to copy
   @param end   last offset to copy (exclusive)
   @return error code (non-positive)
   @retval 0   on success
*/
int copy_mmap(const void *map, int dst, uint64_t start, uint64_t end);
#  define copy_file_mmap copy_mmap
# endif

# ifdef __linux__
#  ifdef __cplusplus
extern "C"
#  endif
/**
   Try to copy a portion of a file via copy_file_range(2).
   @param src   source file descriptor
   @param dst   target to append src to
   @param start first offset to copy
   @param end   last offset to copy (exclusive)
   @return error code (non-positive)
   @retval 0   on success
   @retval 1   if a fallback to copy_mmap() or backup::copy() is needed
*/
int copy_file_range_try(int src, int dst, uint64_t start, uint64_t end);
#  define copy_file_shortcut copy_file_range_try
# endif
#endif

#ifdef __cplusplus
extern "C"
#endif
/** Append to the configuration file.
@param target   backup target directory
@param config   the configuration file snippet to append
@param size     length of the snippet
@return error code (non-positive)
@retval 0   on success */
int backup_config_append(IF_WIN(const char*, int) target,
                         const char *config, size_t size);

#ifdef __cplusplus
extern "C"
#endif
/** Append to the configuration file.
@param target   backup stream
@param config   the configuration file snippet to append
@param size     length of the snippet
@return error code (non-positive)
@retval 0   on success */
int backup_stream_config(backup_fd stream, const char *config, size_t size);

#ifdef __cplusplus
extern "C"
#endif
/** Start streaming a file.
@param target   backup target
@param name     file name
@param mode     file access mode
@param size     physical length of the file, in bytes
@param chunks   payload chunks of a sparse file, or nullptr
@param n_chunks number of chunks; 0 unless sparse file
@return error code (non-positive)
@retval 0   on success */
int backup_stream_start(backup_fd stream,
                        const char *name, mode_t mode, uint64_t size,
                        const struct backup_chunk *chunks, size_t n_chunks);

#ifdef __cplusplus
extern "C"
#endif
/**
   Write data to a stream.
   @param stream  backup stream
   @param buf     source buffer
   @param size    length of the buffer (usually an integer multiple of 512)
   @return error code (non-positive)
   @retval 0 on success
*/
int backup_stream_write(backup_fd stream, const void *buf, size_t size);

#ifdef __cplusplus
extern "C"
#endif
/**
   Append a file snippet to the stream,
   after a corresponding call to backup_stream_start().

   Note that tar uses 512-byte blocks. If end-start is not a multiple of
   512 bytes, backup_stream_zeropad() must be invoked.
   @param src    source file
   @param stream backup stream
   @param start  first offset to copy
   @param end    last offset to copy (exclusive)
   @return error code (non-positive)
   @retval 0   on success
*/
int backup_stream_append_plain(backup_fd src, backup_fd stream,
                               uint64_t start, uint64_t end);

#ifdef __cplusplus
extern "C"
#endif
/**
   Zero-pad a the stream to a multiple of 512 bytes.

   @param stream  backup stream
   @param written least significant bits of the number of payload appended
   @return error code (non-positive)
   @retval 0   on success
*/
int backup_stream_zeropad(backup_fd stream, size_t written);

#ifdef _WIN32
# define backup_stream_append_async backup_stream_append_plain
#else
# ifdef __cplusplus
extern "C"
# endif
/**
   Zero-copy append an immutable file snippet to a stream.

   The caller guarantees that this section of the file will remain
   intact until the stream is closed. This guarantee is needed
   because the receiving end of the stream pipe might delay consuming
   the data, and the operating system might point the pipe buffer
   to the block cache for a long time.

   Note that tar uses 512-byte blocks. If end-start is not a multiple of
   512 bytes, backup_stream_zeropad() must be invoked.
   @param src    source file
   @param stream backup stream
   @param start  first offset to copy
   @param end    last offset to copy (exclusive)
   @return error code (non-positive)
   @retval 0   on success
*/
int backup_stream_append_async(int src, int stream,
                               uint64_t start, uint64_t end);
#endif
