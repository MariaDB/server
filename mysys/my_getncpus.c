/*
   Copyright (c) 2006, 2010, Oracle and/or its affiliates

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

/* get the number of (online) CPUs */

#include "mysys_priv.h"
#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif

#if defined(__FreeBSD__) && defined(HAVE_PTHREAD_GETAFFINITY_NP)
#include <pthread_np.h>
#include <sys/cpuset.h>
#endif

static int ncpus=0;

int my_getncpus(void)
{
#if (defined(__linux__) || defined(__FreeBSD__)) && defined(HAVE_PTHREAD_GETAFFINITY_NP)
  cpu_set_t set;

  if (!ncpus)
  {
    if (pthread_getaffinity_np(pthread_self(), sizeof(set), &set) == 0)
    {
      ncpus= CPU_COUNT(&set);
    }
    else
    {
#ifdef _SC_NPROCESSORS_ONLN
      ncpus= sysconf(_SC_NPROCESSORS_ONLN);
#else
      ncpus= 2;
#endif
    }
  }

#else /* __linux__ || FreeBSD && HAVE_PTHREAD_GETAFFINITY_NP */

  if (!ncpus)
  {
#ifdef _SC_NPROCESSORS_ONLN
    ncpus= sysconf(_SC_NPROCESSORS_ONLN);
#elif defined(__WIN__)
    SYSTEM_INFO sysinfo;

    /*
    * We are not calling GetNativeSystemInfo here because (1) we
    * don't believe that they return different values for number
    * of processors and (2) if WOW64 limits processors for Win32
    * then we don't want to try to override that.
    */
    GetSystemInfo(&sysinfo);

    ncpus= sysinfo.dwNumberOfProcessors;
#else
    /* unknown so play safe: assume SMP and forbid uniprocessor build */
    ncpus= 2;
#endif
  }

#endif /* __linux__ || FreeBSD && HAVE_PTHREAD_GETAFFINITY_NP */

  return ncpus;
}
