/* Copyright (c) 2000, 2014, Oracle and/or its affiliates.
   Copyright (c) 2011, 2016, MariaDB

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

/*****************************************************************************
** The following is a simple implementation of posix conditions
*****************************************************************************/
#if defined(_WIN32)

#undef SAFE_MUTEX			/* Avoid safe_mutex redefinitions */
#include "mysys_priv.h"
#include <m_string.h>
#include <process.h>
#include <sys/timeb.h>


/**
  Convert abstime to milliseconds
*/

static DWORD get_milliseconds(const struct timespec *abstime)
{
  struct timespec current_time;
  long long ms;

  if (abstime == NULL)
    return INFINITE;

  set_timespec_nsec(current_time, 0);
  ms= (abstime->tv_sec - current_time.tv_sec)*1000LL +
    (abstime->tv_nsec - current_time.tv_nsec)/1000000LL;
  if(ms < 0 )
    ms= 0;
  if(ms > UINT_MAX)
    ms= INFINITE;
  return (DWORD)ms;
}

int pthread_cond_init(pthread_cond_t *cond, const pthread_condattr_t *attr)
{
  InitializeConditionVariable(cond);
  return 0;
}


int pthread_cond_destroy(pthread_cond_t *cond)
{
  return 0;
}


int pthread_cond_broadcast(pthread_cond_t *cond)
{
  WakeAllConditionVariable(cond);
  return 0;
}


int pthread_cond_signal(pthread_cond_t *cond)
{
  WakeConditionVariable(cond);
  return 0;
}


int pthread_cond_timedwait(pthread_cond_t *cond, pthread_mutex_t *mutex,
  const struct timespec *abstime)
{
  DWORD timeout= get_milliseconds(abstime);
  if (!SleepConditionVariableCS(cond, mutex, timeout))
    return ETIMEDOUT;
  return 0;  
}


int pthread_cond_wait(pthread_cond_t *cond, pthread_mutex_t *mutex)
{
  return pthread_cond_timedwait(cond, mutex, NULL);
}


int pthread_attr_init(pthread_attr_t *connect_att)
{
  connect_att->dwStackSize	= 0;
  connect_att->dwCreatingFlag	= 0;
  return 0;
}

int pthread_attr_setstacksize(pthread_attr_t *connect_att,DWORD stack)
{
  connect_att->dwStackSize=stack;
  return 0;
}

int pthread_attr_destroy(pthread_attr_t *connect_att)
{
  bzero((uchar*) connect_att,sizeof(*connect_att));
  return 0;
}

#endif /* __WIN__ */
