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


#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <inttypes.h>
#include "append_cache.h"
#include "my_sys.h"


int open_cache(PMEM_APPEND_CACHE *cache, PMEM_APPEND_CACHE_DIRECTORY *dir,
               uint64_t n);

typedef struct st_command
{
  const char *name;
  const char *description;
  int (*func)(int argc, char *argv[]);
} COMMAND;


static int help(int argc, char *argv[]);


static int info(int argc, char *argv[])
{
  PMEM_APPEND_CACHE_DIRECTORY dir;

  if (argc != 3)
  {
    help(argc, argv);
    return 1;
  }
  if (pmem_append_cache_open(&dir, argv[2]))
  {
    perror("Failed to open cache");
    return 1;
  }
  printf("Number of slots in directory: %" PRIu32 ", mapped size: %zu\n",
         dir.header->n_caches,
         dir.mapped_length);
  for (uint32_t i= 0; i < dir.header->n_caches; i++)
  {
    PMEM_APPEND_CACHE cache;
    printf("  cache %" PRIu32 " at offset %" PRIu64 ": ", i,
           dir.start_offsets[i]);
    if (open_cache(&cache, &dir, i))
      printf("failed to open\n");
    else
      printf("buffer size: %" PRIu64
             ", flushed eof: %" PRIu64
             ", cached eof: %" PRIu64
             ", file name length: %" PRIu64
             ", target file name: %s\n",
             cache.buffer_size, cache.header->flushed_eof,
             cache.header->cached_eof, cache.header->file_name_length,
             cache.header->file_name_length ? cache.file_name : "<not attached>");
  }
  if (pmem_append_cache_close(&dir))
  {
    perror("Failed to close cache");
    return 1;
  }
  return 0;
}


static int create(int argc, char *argv[])
{
  if (argc != 5)
  {
    help(argc, argv);
    return 1;
  }
  if (pmem_append_cache_create(argv[2], strtoull(argv[3], 0, 0),
                               strtoul(argv[4], 0, 0)))
  {
    perror("Failed to create cache");
    return 1;
  }
  return 0;
}


static int test(int argc, char *argv[])
{
  PMEM_APPEND_CACHE_DIRECTORY dir;
  PMEM_APPEND_CACHE cache;
  File fd;
  int res= 1;

  if (argc != 4)
  {
    help(argc, argv);
    return 1;
  }
  if ((fd= my_open(argv[3], O_CREAT | O_WRONLY, MYF(MY_WME))) < 0)
  {
    perror("Failed to open target file");
    return 1;
  }
  if (pmem_append_cache_create(argv[2], 64, 1))
  {
    perror("Failed to create cache");
    goto err1;
  }
  if (pmem_append_cache_open(&dir, argv[2]))
  {
    perror("Failed to open cache");
    goto err2;
  }
  if (pmem_append_cache_attach(&cache, &dir, 0, fd, argv[3]))
  {
    perror("Failed to attach to append cache");
    goto err3;
  }
  for (int i= 0; i < 6; i++)
  {
    char buf[16];
    sprintf(buf, "%06d\n", i);
    cache.write(&cache, buf, 7, MYF(0));
  }
  printf("Buffer size: %zd, flushed_eof: %ld, cached_eof: %ld, reserved_eof: %ld\n",
         cache.buffer_size, cache.flushed_eof, cache.cached_eof,
         cache.reserved_eof);
  if (pmem_append_cache_detach(&cache))
  {
    perror("Failed to detach from append cache");
    goto err3;
  }
  res= 0;
err3:
  if (pmem_append_cache_close(&dir))
  {
    perror("Failed to close cache");
    res= 1;
  }
err2:
  if (my_delete(argv[2], MYF(MY_WME)))
  {
    perror("Failed to unlink cache file");
    res= 1;
  }
err1:
  if (my_close(fd, MYF(MY_WME)))
  {
    perror("Failed to close target file");
    res= 1;
  }
  return res;
}


static int flush(int argc, char *argv[])
{
  int res;
  PMEM_APPEND_CACHE_DIRECTORY dir;
  if (argc != 3)
  {
    help(argc, argv);
    return 1;
  }
  if (pmem_append_cache_open(&dir, argv[2]))
  {
    perror("Failed to open cache");
    return 1;
  }
  if ((res= pmem_append_cache_flush(&dir)))
    perror("Failed to flush cache");
  if (pmem_append_cache_close(&dir))
  {
    perror("Failed to close cache");
    return 1;
  }
  return res ? 1 : 0;
}


static COMMAND commands[]=
{
  { "help", "", help },
  { "info", "<path>", info },
  { "create", "<path> <size> <n_caches>", create },
  { "flush", "<path>", flush },
  { "test", "<path> <file_path>", test },
};


static int help(int argc, char *argv[])
{
  puts("usage:");
  for (uint32_t i= 0; i < sizeof(commands) / sizeof(commands[0]); i++)
    printf("  pmemac %s %s\n", commands[i].name, commands[i].description);
  return 0;
}


int main(int argc, char *argv[])
{
  if (argc > 1)
  {
    for (uint32_t i= 0; i < array_elements(commands); i++)
    {
      if (!strcmp(commands[i].name, argv[1]))
      {
        int res;
        MY_INIT(argv[0]);
        res= commands[i].func(argc, argv);
        my_end(0);
        return res;
      }
    }
  }
  help(argc, argv);
  return 1;
}
