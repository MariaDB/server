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


#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <libpmem.h>
#include "append_cache.h"
#include "my_cpu.h"
#include "mysql/psi/mysql_file.h"


#define PMEM_APPEND_CACHE_MIN_SIZE 8192


/* PMAC0\0\0\0 */
static const uint32_t pmem_append_cache_magic= 0x010dfefe;


/**
  Calculates full directory header size.

  @param n_caches  number of caches

  @return full directory header size
*/

static uint64_t directory_header_size(uint32_t n_caches)
{
  return sizeof(PMEM_APPEND_CACHE_DIRECTORY_HEADER) +
         sizeof(uint64_t) * n_caches;
}


/**
  Creates and initialises new append cache file.

  @param dir       cache directory
  @param path      path to cache file
  @param size      size of cache file in bytes
  @param n_caches  number of caches in directory

  Upon successful completion new file is created, file header is initialised and
  the rest of the file is initialised with null bytes, dir is initialised and
  becomes usable.

  File signature is written last, so that half-initialised file is not usable.

  Existing files are not overwritten, an error is returned instead.

  Caller is responsible for closing dir.

  @return
    @retval 0 success
    @retval -1 failure
*/

static int create_directory(PMEM_APPEND_CACHE_DIRECTORY *dir,
                            const char *path, uint64_t size, uint32_t n_caches)
{
  const uint64_t header_size= directory_header_size(n_caches);
  uint64_t start_offset= header_size;
  uint64_t cache_size;

  if (size < header_size || !n_caches)
    goto err;

  cache_size= ((size - header_size) / n_caches) & ~(uint64_t) 7;
  if (cache_size < PMEM_APPEND_CACHE_MIN_SIZE)
  {
err:
    dir->header= 0;
    return -1;
  }

  if (!(dir->header= pmem_map_file(path, size,
                                   PMEM_FILE_CREATE | PMEM_FILE_EXCL,
                                   S_IRUSR | S_IWUSR, &dir->mapped_length, 0)))
    return -1;

  dir->start_offsets= (void*) (dir->header + 1);
  for (uint32_t i= 0; i < n_caches; i++)
  {
    dir->start_offsets[i]= start_offset;
    start_offset+= cache_size;
  }
  dir->header->n_caches= n_caches;
  pmem_persist(dir->header, header_size);

  dir->header->magic= pmem_append_cache_magic;
  pmem_persist(&dir->header->magic, sizeof(dir->header->magic));

  return 0;
}


/**
  Initialises append cache basing on n-th directory slot.

  @param dir         cache directory
  @param cache       cache descriptor
  @param cache_slot  cache slot

  @return
    @retval 0 success
    @retval -1 failure
*/

int open_cache(PMEM_APPEND_CACHE *cache, PMEM_APPEND_CACHE_DIRECTORY *dir,
               uint32_t cache_slot)
{
  uint64_t cache_start;
  uint64_t cache_end;

  DBUG_ASSERT(cache_slot < dir->header->n_caches);
  cache_start= dir->start_offsets[cache_slot];
  cache_end= cache_slot == dir->header->n_caches - 1 ?
             dir->mapped_length : dir->start_offsets[cache_slot + 1];

  if (cache_start < directory_header_size(dir->header->n_caches) ||
      cache_start > cache_end ||
      cache_start & 7 ||
      cache_end - cache_start < PMEM_APPEND_CACHE_MIN_SIZE ||
      cache_end > dir->mapped_length)
    return -1;

  cache->header= (void*) dir->header + cache_start;
  cache->file_name= (void*) (cache->header + 1);
  cache->buffer= cache->file_name + cache->header->file_name_length;
  cache->buffer_size= cache_end - cache_start -
                      sizeof(PMEM_APPEND_CACHE_HEADER) -
                      cache->header->file_name_length;
  cache->flushed_eof= cache->header->flushed_eof;
  cache->cached_eof= cache->header->cached_eof;
  cache->reserved_eof= cache->cached_eof;

  if (cache->header->file_name_length >=
      cache_end - cache_start - sizeof(PMEM_APPEND_CACHE_HEADER) ||
      cache->cached_eof < cache->flushed_eof ||
      cache->cached_eof - cache->flushed_eof > cache->buffer_size)
    return -1;

  return 0;
}


/**
  Flushes append cache.

  @param cache  cache descriptor

  Appends as much cached data as available and advances flushed_eof.
  This function cannot be called concurrently with itself and is intended to be
  used by flusher_thread().

  Other threads can update cache->cached_eof concurrently.
  No other thread can change cache->flushed_eof.

  @return
    @retval 0 success
    @retval -1 write failed
*/

static int flush_cache(PMEM_APPEND_CACHE *cache)
{
  uint64_t cached_eof;
  uint64_t flushed_eof= cache->flushed_eof;

  /*
    Check if we have anything to flush. The atomic operation is only to ensure
    that the compiler doesn't try to optimize the read away.
  */
  while (flushed_eof <
         (cached_eof= (uint64_t) my_atomic_load64_explicit(
                      (int64*) &cache->cached_eof, MY_MEMORY_ORDER_RELAXED)))
  {
    uint64_t write_size;
    size_t written;
    ssize_t flush_offset= flushed_eof % cache->buffer_size;

    if (cached_eof / cache->buffer_size == flushed_eof / cache->buffer_size)
    {
      /* Flush everything that not aleady flushed in the cache */
      write_size= cached_eof - flushed_eof;
    }
    else
    {
      /* Nothing in the cache is flushed. Flush everything */
      write_size= cache->buffer_size - flush_offset;
    }
    if ((written= mysql_file_pwrite(cache->file_fd,
                                    cache->buffer + flush_offset,
                                    write_size, flushed_eof,
                                    MYF(MY_WME))) == MY_FILE_ERROR)
      return -1;
    if (mysql_file_sync(cache->file_fd, MYF(MY_WME)))
      return -1;
    flushed_eof+= written;

    /* Store the new flushed position in the directory */
    my_atomic_store64_explicit((int64*) &cache->header->flushed_eof,
                               (int64) flushed_eof, MY_MEMORY_ORDER_RELAXED);
    pmem_persist(&cache->header->flushed_eof,
                 sizeof(cache->header->flushed_eof));

    /* Store the new flushed position in the cache */
    my_atomic_store64_explicit((int64*) &cache->flushed_eof,
                               (int64) flushed_eof, MY_MEMORY_ORDER_RELAXED);
  }
  return 0;
}


/**
  Background flusher thread.

  @param cache  cache descriptor
*/

static void *flusher_thread(void *arg)
{
  PMEM_APPEND_CACHE_DIRECTORY *dir= (PMEM_APPEND_CACHE_DIRECTORY*) arg;
  pthread_mutex_lock(&dir->mutex);
  while (!dir->stop_flusher)
  {
    for (LIST *list= dir->caches; list; list= list->next)
    {
      if (flush_cache((PMEM_APPEND_CACHE*) list->data))
        abort();
    }
    pthread_mutex_unlock(&dir->mutex);
    my_sleep(1000);
    pthread_mutex_lock(&dir->mutex);
  }
  pthread_mutex_unlock(&dir->mutex);
  return 0;
}


static int init_directory(PMEM_APPEND_CACHE_DIRECTORY *dir)
{
  pthread_mutex_init(&dir->mutex, 0);
  dir->caches= 0;
  dir->stop_flusher= false;
  if (pthread_create(&dir->flusher_thread, 0, flusher_thread, dir))
  {
    pthread_mutex_destroy(&dir->mutex);
    pmem_unmap(dir->header, dir->mapped_length);
    dir->header= 0;
    return -1;
  }
  return 0;
}


/**
  Writes data via append cache.

  @param cache   cache descriptor
  @param data    buffer to write
  @param length  length of data

  @return number of bytes written
*/

static size_t cache_write(PMEM_APPEND_CACHE *cache, const void *data,
                          size_t length, myf flags)
{
  if (length)
  {
    /* Reserve space for this write */
    uint64_t start=
      (uint64_t) my_atomic_add32_explicit((int64*) &cache->reserved_eof, length,
                                          MY_MEMORY_ORDER_RELAXED);
    uint64_t write_pos= start;
    size_t left= length;

    /* Copy data chunks */
    do
    {
      uint64_t chunk_offset= write_pos % cache->buffer_size;
      uint64_t used, avail;

      /* Wait for flusher thread to release some space */
      while ((used= write_pos -
              (uint64_t) my_atomic_load64_explicit((int64*) &cache->flushed_eof,
                                                   MY_MEMORY_ORDER_RELAXED)) >=
             cache->buffer_size)
        LF_BACKOFF();

      avail= cache->buffer_size - used;
      if (avail > left)
        avail= left;

      if (avail > cache->buffer_size - chunk_offset)
        avail= cache->buffer_size - chunk_offset;

      pmem_memcpy_persist(cache->buffer + chunk_offset, data, avail);

      left-= avail;
      data+= avail;
      write_pos+= avail;

      /*
        Wait for preceding concurrent writes completion. This is required to ensure
        we don't have a hole of not written data in the middle of the cache when
        we update cached_eof
      */
      while ((uint64_t) my_atomic_load64_explicit((int64*) &cache->cached_eof,
                                                  MY_MEMORY_ORDER_RELAXED) <
             start)
        LF_BACKOFF();
      /* Commit this write */
      my_atomic_store64_explicit((int64*) &cache->header->cached_eof,
                                 (int64) write_pos, MY_MEMORY_ORDER_RELAXED);
      pmem_persist(&cache->header->cached_eof,
                   sizeof(cache->header->cached_eof));
      my_atomic_store64_explicit((int64*) &cache->cached_eof,
                                 (int64) write_pos, MY_MEMORY_ORDER_RELAXED);
    } while (left);
  }
  return flags & (MY_NABP | MY_FNABP) ? 0 : length;
}


static size_t no_cache_write(PMEM_APPEND_CACHE *cache, const void *data,
                             size_t length, myf flags)
{
  return mysql_file_write(cache->file_fd, data, length, flags);
}


/**
  Waits until cache is flushed to a file up to this offset.

  @param cache   cache descriptor
  @param offset  sync up to this offset

  If offset is 0, waits until all cached data as of call time is flushed.
*/

static void cache_flush(PMEM_APPEND_CACHE *cache, uint64_t offset)
{
  if (!offset)
    offset= (uint64_t) my_atomic_load64_explicit((int64*) &cache->cached_eof,
                                                 MY_MEMORY_ORDER_RELAXED);
  while ((uint64_t) my_atomic_load64_explicit((int64*) &cache->flushed_eof,
                                              MY_MEMORY_ORDER_RELAXED) < offset)
    LF_BACKOFF();
}


static void no_cache_flush(PMEM_APPEND_CACHE *cache, uint64_t offset)
{
}


static int cache_sync(PMEM_APPEND_CACHE *cache, myf flags)
{
  return 0;
}


static int no_cache_sync(PMEM_APPEND_CACHE *cache, myf flags)
{
  return mysql_file_sync(cache->file_fd, flags);
}


/**
  Creates and initialises new append cache file.

  @param path      path to cache file
  @param size      size of cache file in bytes
  @param n_caches  number of caches in directory

  A wrapper around create_directory().

  @return
    @retval 0 success
    @retval -1 failure
*/

int pmem_append_cache_create(const char *path, uint64_t size,
                             uint32_t n_caches)
{
  PMEM_APPEND_CACHE_DIRECTORY dir;
  int res= create_directory(&dir, path, size, n_caches);

  if (!res && (res= pmem_unmap(dir.header, dir.mapped_length)))
    my_delete(path, MYF(MY_WME));
  return res;
}


/**
  Opens append cache file.

  @param dir   cache directory
  @param path  path to cache file

  @return
    @retval 0 success
    @retval -1 failure
*/

int pmem_append_cache_open(PMEM_APPEND_CACHE_DIRECTORY *dir, const char *path)
{
  if (!(dir->header= pmem_map_file(path, 0, 0, 0, &dir->mapped_length, 0)))
    return -1;

  if (init_directory(dir))
    return -1;

  if (dir->mapped_length < sizeof(PMEM_APPEND_CACHE_DIRECTORY_HEADER) ||
      dir->header->magic != pmem_append_cache_magic ||
      !dir->header->n_caches ||
      dir->header->n_caches > ((dir->mapped_length -
                                sizeof(PMEM_APPEND_CACHE_DIRECTORY_HEADER)) /
                               sizeof(uint64_t)))
  {
    pmem_append_cache_close(dir);
    return -1;
  }

  dir->start_offsets= (void*) (dir->header + 1);
  return 0;
}


/**
  Closes append cache file.

  @param dir  cache directory

  All cache slots must be in a detached state by the time of this function call.

  @return
    @retval 0 success
    @retval -1 failure
*/

int pmem_append_cache_close(PMEM_APPEND_CACHE_DIRECTORY *dir)
{
  if (dir->header)
  {
    int res= pmem_unmap(dir->header, dir->mapped_length);
    dir->header= 0;
    DBUG_ASSERT(!dir->caches);
    pthread_mutex_lock(&dir->mutex);
    dir->stop_flusher= true;
    pthread_mutex_unlock(&dir->mutex);
    pthread_join(dir->flusher_thread, 0);
    pthread_mutex_destroy(&dir->mutex);
    return res;
  }
  return 0;
}


/**
  Flushes all caches in directory.

  @param dir  cache directory

  @return
    @retval 0 success
    @retval -1 failure
*/

int pmem_append_cache_flush(PMEM_APPEND_CACHE_DIRECTORY *dir)
{
  int res= 0;

  for (uint32_t i= 0; i < dir->header->n_caches; i++)
  {
    PMEM_APPEND_CACHE cache;
    MY_STAT sb;

    if (open_cache(&cache, dir, i))
      res= -1;
    else if (!cache.header->file_name_length)
    {
    }
    else if (cache.file_name[cache.header->file_name_length])
      res= -1;
    else if (cache.header->flushed_eof == cache.header->cached_eof)
    {
      /* Nothing to do. Remove cached file from append_cache */
      cache.header->file_name_length= 0;
      pmem_persist(&cache.header->file_name_length,
                   sizeof(cache.header->file_name_length));
    }
    else if ((cache.file_fd= my_open(cache.file_name, O_WRONLY,
                                     MYF(MY_WME))) < 0)
      res= -1;
    else
    {
      /*
        Something is wrong with the file as our directory tells us that
        we have flushed more data than what is in the file.
      */
      if (my_fstat(cache.file_fd, &sb, MYF(MY_WME)) ||
          cache.header->flushed_eof > (uint64_t) sb.st_size ||
          flush_cache(&cache))
        res= -1;
      if (my_close(cache.file_fd, MYF(MY_WME)))
        res= -1;
    }
  }
  return res;
}


/**
  Initialises append cache directory.

  @param dir       cache directory
  @param path      path to cache file
  @param size      size of cache file in bytes
  @param n_caches  number of caches in directory

  If path exists: opens append cache directory and flushes all caches,
  size and n_caches are ignored.

  If path doesn't exist: creates new append cache directory.

  @return
    @retval 0 success
    @retval -1 failure
*/

int pmem_append_cache_init(PMEM_APPEND_CACHE_DIRECTORY *dir, const char *path,
                           uint64_t size, uint32_t n_caches)
{
  int res;
  if (!path)
  {
    dir->header= 0;
    return 0;
  }
  if (!my_access(path, F_OK))
  {
    if (!pmem_append_cache_open(dir, path))
    {
      if (!pmem_append_cache_flush(dir))
      {
        if (dir->header->n_caches == n_caches && dir->mapped_length == size)
          return 0;
        pmem_append_cache_close(dir);
        my_delete(path, MYF(0));
        goto create;
      }
      pmem_append_cache_close(dir);
    }
    return -1;
  }
create:
  if (!(res= create_directory(dir, path, size, n_caches)))
    res= init_directory(dir);
  return res;
}


/**
  Attaches append cache to n-th directory slot.

  @param cache      cache descriptor
  @param dir        cache directory
  @param cache_slot cache slot
  @param file_fd    file descriptor
  @param file_name  name of file

  Upon successful completion cache becomes usable.

  It is only possible to attach to detached cache slot, that is only if
  file_name_length is 0.

  @return
    @retval 0 success
    @retval -1 failure
*/

int pmem_append_cache_attach(PMEM_APPEND_CACHE *cache,
                             PMEM_APPEND_CACHE_DIRECTORY *dir,
                             uint32_t cache_slot,
                             File file_fd,
                             const char *file_name)
{
  MY_STAT sb;
  size_t file_name_length;
  int res;

  cache->file_fd= file_fd;

  if (!dir || !dir->header)
  {
    cache->write= no_cache_write;
    cache->flush= no_cache_flush;
    cache->sync= no_cache_sync;
    return 0;
  }
  else
  {
    cache->write= cache_write;
    cache->flush= cache_flush;
    cache->sync= cache_sync;
  }

  if (my_fstat(file_fd, &sb, MYF(MY_WME)))
    return -1;

  if ((res= open_cache(cache, dir, cache_slot)))
    return res;

  file_name_length= strlen(file_name) + 1;
  if (cache->header->file_name_length ||
      file_name_length >= cache->buffer_size)
    return -1;

  cache->header->flushed_eof= cache->header->cached_eof=
  cache->flushed_eof= cache->cached_eof= cache->reserved_eof= sb.st_size;

  memcpy(cache->file_name, file_name, file_name_length);
  /* file name is guaranteed to always be after the header */
  pmem_persist(cache->header, sizeof(cache->header) + file_name_length);

  cache->header->file_name_length= file_name_length;
  pmem_persist(&cache->header->file_name_length,
               sizeof(cache->header->file_name_length));

  cache->buffer+= file_name_length;
  cache->buffer_size-= file_name_length;

  cache->element.data= cache;
  cache->dir= dir;
  pthread_mutex_lock(&dir->mutex);
  dir->caches= list_add(dir->caches, &cache->element);
  pthread_mutex_unlock(&dir->mutex);
  return 0;
}


/**
  Detaches append cache from directory slot.

  @param cache  cache descriptor

  Flushes cached data, marks directory slot free by resetting file_name_length,
  cache becomes unusable.

  Cache must not be accessed concurrently for the duration of this function
  call.

  If not all cached data was flushed, directory slot is not released
  (file_name_length is not reset) and an error is returned.

  @return
    @retval 0 success
    @retval -1 failure
*/

int pmem_append_cache_detach(PMEM_APPEND_CACHE *cache)
{
  int res;

  if (cache->write == no_cache_write)
    return 0;

  pthread_mutex_lock(&cache->dir->mutex);
  cache->dir->caches= list_delete(cache->dir->caches, &cache->element);
  pthread_mutex_unlock(&cache->dir->mutex);

  if (!(res= flush_cache(cache)))
  {
    cache->header->file_name_length= 0;
    pmem_persist(&cache->header->file_name_length,
                 sizeof(cache->header->file_name_length));
  }
  return res;
}
