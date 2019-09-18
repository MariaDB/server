#ifndef APPEND_CACHE_H_INCLUDED
#define APPEND_CACHE_H_INCLUDED
/*
  Copyright (c) 2019, MariaDB Corporation.

  This program is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation; version 2 of the License.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program; if not, write to the Free Software
  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1335  USA
*/


#include <stdint.h>
#include <stdbool.h>
#include <my_global.h>
#include <my_list.h>


#ifdef __cplusplus
extern "C"
{
#endif


/*
  Cache file consists of directory header followed by zero or more append
  caches.

  Directory header:
  magic            (4 bytes)                file signature
  n_caches         (4 bytes)                number of caches in directory
  start_offsets    (8 bytes * n_caches)     array of cache start offsets from
                                            the beginning of the cache file

  Append cache format:
  flushed_eof      (8 bytes)                cache is flushed up to this offset
  cached_eof       (8 bytes)                cache contains data up to this offset
  file_name_length (8 bytes)                file name length
  file_name        (file_name_length bytes) target file name
  buffer           (N)                      circular buffer, lasts up to the
                                            following cache start offset or eof
                                            for the last cache

  flushed_eof normally equals to the target file size. It is an error if
  flushed_eof is smaller than the target file size.

  flushed_eof can be larger than the target file size if crash occured while
  flushing append cache. In such case data after flushed_eof is overwritten
  during crash recovery.

  Append cache is holding data, which otherwise would be stored in the target
  file between flushed_eof and cached_eof.

  flushed_eof is advanced by data size whenever data is added. cached_eof is
  advanced by flushed data size whenever data is flushed.

  Cached data starts at buffer + flushed_eof % buffer_size and ends at
  buffer + cached_eof % buffer_size.
*/


/** Fixed-size directory header. */
typedef struct st_pmem_append_cache_directory_header
{
  uint32_t magic;
  uint32_t n_caches;
} PMEM_APPEND_CACHE_DIRECTORY_HEADER;


/** In-memory cache directory descriptor. */
typedef struct st_pmem_append_cache_directory
{
  LIST *caches;
  pthread_t flusher_thread;
  pthread_mutex_t mutex;
  PMEM_APPEND_CACHE_DIRECTORY_HEADER *header;
  uint64_t *start_offsets;
  size_t mapped_length;
  bool stop_flusher;
} PMEM_APPEND_CACHE_DIRECTORY;


/** Fixed-size cache header. */
typedef struct st_pmem_append_cache_header
{
  uint64_t flushed_eof;
  uint64_t cached_eof;
  uint64_t file_name_length;
} PMEM_APPEND_CACHE_HEADER;


/** In-memory append cache descriptor. */
typedef struct st_pmem_append_cache
{
  LIST element;
  PMEM_APPEND_CACHE_DIRECTORY *dir;
  PMEM_APPEND_CACHE_HEADER *header;
  char *file_name;
  void *buffer;
  uint64_t buffer_size;
  File file_fd;

  size_t (*write)(struct st_pmem_append_cache *cache, const void *data,
                  size_t length, myf flags);
  void (*flush)(struct st_pmem_append_cache *cache, uint64_t offset);
  int (*sync)(struct st_pmem_append_cache *cache, uint64_t offset);

  char pad1[CPU_LEVEL1_DCACHE_LINESIZE];
  uint64_t flushed_eof;
  char pad2[CPU_LEVEL1_DCACHE_LINESIZE];
  uint64_t cached_eof;
  char pad3[CPU_LEVEL1_DCACHE_LINESIZE];
  uint64_t reserved_eof;
  char pad4[CPU_LEVEL1_DCACHE_LINESIZE];
} PMEM_APPEND_CACHE;


int pmem_append_cache_create(const char *path, uint64_t size,
                             uint32_t n_caches);
int pmem_append_cache_open(PMEM_APPEND_CACHE_DIRECTORY *dir, const char *path);
int pmem_append_cache_close(PMEM_APPEND_CACHE_DIRECTORY *dir);
int pmem_append_cache_flush(PMEM_APPEND_CACHE_DIRECTORY *dir);
int pmem_append_cache_init(PMEM_APPEND_CACHE_DIRECTORY *dir, const char *path,
                           uint64_t size, uint32_t n_caches);
int pmem_append_cache_attach(PMEM_APPEND_CACHE *cache,
                             PMEM_APPEND_CACHE_DIRECTORY *dir,
                             uint32_t cache_slot,
                             File file_fd,
                             const char *file_name);
int pmem_append_cache_detach(PMEM_APPEND_CACHE *cache);

#ifdef __cplusplus
}
#endif
#endif
