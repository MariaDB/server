/* Copyright (c) 2000, 2010, Oracle and/or its affiliates. All rights reserved.

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

/* Open a temporary file and cache it with io_cache. Delete it on close */

#include "mysys_priv.h"
#include <m_string.h>
#include "my_static.h"
#include "mysys_err.h"

/**
  Open tempfile cached by IO_CACHE

  Should be used when no seeks are done (only reinit_io_buff)
  Return 0 if cache is inited ok
  The actual file is created when the IO_CACHE buffer gets filled
  If dir is not given, use TMPDIR.
*/
my_bool open_cached_file(IO_CACHE *cache, const char* dir, const char *prefix,
                         size_t cache_size, myf cache_myflags)
{
  DBUG_ENTER("open_cached_file");
  cache->dir= dir;
  if (prefix)
  {
    DBUG_ASSERT(strlen(prefix) == 2);
    memcpy(cache->prefix, prefix, 3);
  }
  else
    cache->prefix[0]= 0;
  cache->file_name=0;
  cache->buffer=0;				/* Mark that not open */
  if (!init_io_cache(cache, -1, cache_size, WRITE_CACHE, 0L, 0,
		     MYF(cache_myflags | MY_NABP | MY_TRACK)))
  {
    DBUG_RETURN(0);
  }
  DBUG_RETURN(1);
}

/**
  Create the temporary file
*/
my_bool real_open_cached_file(IO_CACHE *cache)
{
  char name_buff[FN_REFLEN];
  int error=1;
  DBUG_ENTER("real_open_cached_file");
  if ((cache->file= create_temp_file(name_buff, cache->dir,
                                    cache->prefix[0] ? cache->prefix : 0,
                                    O_BINARY, MYF(MY_WME | MY_TEMPORARY))) >= 0)
  {
    error=0;
  }
  DBUG_RETURN(error);
}


void close_cached_file(IO_CACHE *cache)
{
  DBUG_ENTER("close_cached_file");
  if (my_b_inited(cache))
  {
    File file= cache->file;
    if (file >= 0)
    {
      (void) my_close(file,MYF(0));
#ifdef CANT_DELETE_OPEN_FILES
      if (cache->file_name)
      {
	(void) my_delete(cache->file_name, MYF(MY_WME));
	my_free(cache->file_name);
      }
#endif
    }
    cache->file= -1;				/* Don't flush data */
    (void) end_io_cache(cache);
  }
  DBUG_VOID_RETURN;
}
