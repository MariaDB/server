/*
   Copyright (c) 2022, MariaDB Corporation.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1335  USA */

#if defined __linux__
# include <malloc.h>
#endif

inline void *aligned_malloc(size_t size, size_t alignment)
{
#ifdef _WIN32
  return _aligned_malloc(size, alignment);
#elif defined __linux__
  return memalign(alignment, size);
#else
  void *result;
  if (posix_memalign(&result, alignment, size))
    result= NULL;
  return result;
#endif
}

inline void aligned_free(void *ptr)
{
  IF_WIN(_aligned_free,free)(ptr);
}
