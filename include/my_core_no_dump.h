/*
   Copyright (c) 2016, 2017, MariaDB Corporation.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA */

/* This is the include file that should be included 'first' in every C file. */

#ifndef MY_CORE_NO_DUMP_INCLUDED
#define MY_CORE_NO_DUMP_INCLUDED

/*
 Core dump control options to exclude certain buffers from core dump files

 There are two motivations for excluding things from core dumps:

 * resource utilization: stuff like the InnoDB pool buffer is rarely needed
   for post mortem debugging, but on machines with large amounts of memory
   just the time and file system space it takes to write a core dump can
   become substantial. Large core dump sizes can also become an obstacle
   when providing a core dump to a 3rd party for analysis

 * security: certain buffers, especially the InnoDB pool buffer or the
   Aria page cache, are likely to contain sensitive user data. Excluding
   these from a core dump can improve data security and especially can be
   a requirement for being able to pass production server core dumps to
   3rd parties for analysis
*/

#include <my_global.h>

#ifdef HAVE_MADV_DONTDUMP

#include <sys/mman.h>

/* Core dump exclusion constants */
#define CORE_NODUMP_NONE                (0)
#define CORE_NODUMP_INNODB_POOL_BUFFER  (1 << 1)
#define CORE_NODUMP_MYISAM_KEY_BUFFER   (1 << 2)
#define CORE_NODUMP_MAX                 (255)

extern ulong opt_core_nodump;

inline void exclude_from_coredump(void *ptr, size_t size, ulong flags)
{
  if (opt_core_nodump & flags) {
    madvise(ptr, size, MADV_DONTDUMP);
  }
}

#else /* HAVE_MADV_DONTDUMP */

#define exclude_from_coredump(ptr, size, flag)

#endif /* HAVE_MADV_DONTDUMP */

#endif
