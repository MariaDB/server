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
#include "backup_stages.h"
#include <stdint.h>
#include "my_sys.h"

#ifdef _WIN32
struct native_file_handle;
#endif

/** BACKUP SERVER target */
struct backup_target
{
#ifdef _WIN32
  /** Target directory name, or nullptr if streaming (@see backup_sink) */
  const char *path;
  bool operator==(const backup_target &o) const { return path == o.path; }
#else
  /** Target directory descriptor, or -1 if streaming (@see backup_sink) */
  int fd;
  bool operator==(const backup_target &o) const { return fd == o.fd; }
#endif
  /** Options for binlog backup */
  bool binlog_skip;
  bool binlog_minimal;

  /**
     Create a subdirectory in the target directory if it does not exist.
     @param name    subdirectory name
     @return error code (also errno will be set)
     @retval 0 on success (errno might not be touched)
  */
  int mkdir(const char *name) const noexcept;

  /**
     Create a file in the target directory.
     @param name   file name
     @return file handle
     @retval backup_sink::NO_STREAM in case of an error
  */
  my_native_file create_file(const char *name) const noexcept;

  /**
     Set the file position on a native file handle.

     @param fd      file handle from create_file()
     @param offset  seek offset
     @return whether the operation failed
  */
  static bool seek(my_native_file fd, my_off_t offset) noexcept;

  /**
     Close a file handle.
     @param fd  file handle from create_file()
     @return error code
     @retval 0 on success
  */
  static int close(my_native_file fd) noexcept
  { return IF_WIN(!CloseHandle(fd), ::close(fd)); }

  /**
     Append to the configuration file.
     @param target   backup target directory
     @param config   the configuration file snippet to append
     @param size     length of the snippet
     @return error code (non-positive)
     @retval 0   on success */
  int config_append(const char *config, size_t size) const noexcept;
};

/** A payload chunk in a sparse file that is being streamed */
struct backup_chunk
{
  /** byte offset of the start of the payload, from the start of the file */
  uint64_t offset;
  /** length of the hole */
  uint64_t length;
};

/** BACKUP SERVER worker specific context */
struct backup_sink
{
  /** worker identifier: 0 to CONCURRENT-1 (the connection thread) */
  int id;
  /** A value indicating an invalid stream */
  static constexpr my_native_file NO_STREAM{IF_WIN(INVALID_HANDLE_VALUE,-1)};
  /** Target pipe, or NO_STREAM when not streaming */
  my_native_file stream;
  /** storage engine context returned by handlerton::backup_start() */
  void *ha_data;

  /**
     Append to the configuration file.
     @param config   the configuration file snippet to append
     @param size     length of the snippet
     @return error code (non-positive)
     @retval 0   on success
  */
  int config_append(const char *config, size_t size) const noexcept;

  /**
     Start streaming a file.
     @param name     file name
     @param mode     file access mode
     @param size     physical length of the file, in bytes
     @param chunks   payload chunks of a sparse file, or nullptr
     @param n_chunks number of chunks; 0 unless sparse file
     @return error code (non-positive)
     @retval 0   on success
  */
  int create_file(const char *name, mode_t mode, uint64_t size,
                  const backup_chunk *chunks, size_t n_chunks) const noexcept;

  /**
     Write data to a stream or a file.
     @param dst     file or stream to write to
     @param buf     source buffer
     @param size    length of the buffer
     @return error code (non-positive)
     @retval 0 on success
  */
  static int write(my_native_file dst, const void *buf, size_t size) noexcept;

  /**
     Write data to a stream.
     @param buf     source buffer
     @param size    length of the buffer (usually an integer multiple of 512)
     @return error code (non-positive)
     @retval 0 on success
  */
  int write(const void *buf, size_t size) const noexcept
  { return write(stream, buf, size); }

  /**
     Zero-pad a the stream to a multiple of 512 bytes.
     @param written least significant bits of the number of payload appended
     @return error code (non-positive)
     @retval 0   on success
  */
  int zeropad(size_t written) const noexcept;

  /**
     Append a file snippet to the stream,
     after a corresponding call to create_file().

     Note that tar uses 512-byte blocks. If end-start is not a multiple of
     512 bytes, write() or zeropad() must be invoked to pad the output.
     @param src    source file
     @param start  first offset to copy
     @param end    first offset not to copy
     @return error code (non-positive)
     @retval 0   on success
  */
  int append(my_native_file src, uint64_t start, uint64_t end) const noexcept;

#ifdef _WIN32
  int append(native_file_handle src, uint64_t start, uint64_t end)
    const noexcept;
#endif

#ifdef __linux__
  /**
     Zero-copy append an immutable file snippet to a stream.

     The caller guarantees that this section of the file will remain
     intact until the stream is closed. This guarantee is needed
     because the receiving end of the stream pipe might delay consuming
     the data, and the operating system might point the pipe buffer
     to the block cache for a long time.

     Note that tar uses 512-byte blocks. If end-start is not a multiple of
     512 bytes, write() or zeropad() must be invoked afterwards.
     @param src    file that will not be modified until the stream is closed
     @param start  first offset to copy
     @param end    first offset not to copy
     @return error code (non-positive)
     @retval 0   on success
  */
  int append_async(int src, uint64_t start, uint64_t end) const noexcept;
#else
# define append_async(src,start,end) append(src,start,end)
#endif
};

namespace backup {

#ifdef _WIN32
/**
   Copy a portion of a file.
   @param src   source file descriptor
   @param dst   target to append src to
   @param start first offset to copy
   @param end   first offset not to copy
   @return error code (non-positive)
   @retval 0   on success
*/
int copy(native_file_handle src, HANDLE dst, uint64_t start, uint64_t end)
  noexcept;
#else
inline int copy(int src, int dst, uint64_t start, uint64_t end) noexcept
{ return my_copy_file_range(src, dst, start, end); }

/**
   Copy or stream a file.
   @param target     BACKUP SERVER target (possibly, a directory)
   @param sink       per-thread context (possibly, a stream to write to)
   @param src        source file descriptor from open(), to be closed here
   @param path       file name
   @return error code (also errno will be set)
   @retval 0 on success
*/
int copy_or_stream(const backup_target &target, const backup_sink &sink,
                   int src, const char *path) noexcept;
#endif

/**
   Copy or stream a file.
   @param target     BACKUP SERVER target (possibly, a directory)
   @param sink       per-thread context (possibly, a stream to write to)
   @param path       file name
   @param dir_prefix length of the path prefix to omit from the backup
   @return error code (also errno will be set)
   @retval 0 on success
*/
int copy_or_stream(const backup_target &target, const backup_sink &sink,
                   const char *path, size_t dir_prefix= 0) noexcept;

}
