/* Copyright (c) 2000, 2011, Oracle and/or its affiliates. All rights reserved.

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

/* Synchronization - readers / writer thread locks */

#include "mysys_priv.h"
#if defined(NEED_MY_RW_LOCK)
#include <errno.h>

#ifdef _WIN32

int my_rw_init(my_rw_lock_t *rwp)
{
  InitializeSRWLock(&rwp->srwlock);
  rwp->have_exclusive_srwlock = FALSE;
  return 0;
}


int my_rw_rdlock(my_rw_lock_t *rwp)
{
  AcquireSRWLockShared(&rwp->srwlock);
  return 0;
}


int my_rw_tryrdlock(my_rw_lock_t *rwp)
{
  if (!TryAcquireSRWLockShared(&rwp->srwlock))
    return EBUSY;
  return 0;
}


int my_rw_wrlock(my_rw_lock_t *rwp)
{
  AcquireSRWLockExclusive(&rwp->srwlock);
  rwp->have_exclusive_srwlock= TRUE;
  return 0;
}

int my_rw_trywrlock(my_rw_lock_t *rwp)
{
  if (!TryAcquireSRWLockExclusive(&rwp->srwlock))
    return EBUSY;
  rwp->have_exclusive_srwlock= TRUE;
  return 0;
}


int  my_rw_unlock(my_rw_lock_t *rwp)
{
  if (rwp->have_exclusive_srwlock)
  {
    rwp->have_exclusive_srwlock= FALSE;
    ReleaseSRWLockExclusive(&rwp->srwlock);
  }
  else
  {
    ReleaseSRWLockShared(&rwp->srwlock);
  }
  return 0;
}

int my_rw_destroy(my_rw_lock_t* rwp)
{
  DBUG_ASSERT(!rwp->have_exclusive_srwlock);
  return 0;
}

#else

/*
  Source base from Sun Microsystems SPILT, simplified for MySQL use
  -- Joshua Chamas
  Some cleanup and additional code by Monty
*/

/*
*  Multithreaded Demo Source
*
*  Copyright (C) 1995 by Sun Microsystems, Inc.
* 
*
*  This file is a product of SunSoft, Inc. and is provided for
*  unrestricted use provided that this legend is included on all
*  media and as a part of the software program in whole or part.
*  Users may copy, modify or distribute this file at will.
*
*  THIS FILE IS PROVIDED AS IS WITH NO WARRANTIES OF ANY KIND INCLUDING
*  THE WARRANTIES OF DESIGN, MERCHANTIBILITY AND FITNESS FOR A PARTICULAR
*  PURPOSE, OR ARISING FROM A COURSE OF DEALING, USAGE OR TRADE PRACTICE.
*
*  This file is provided with no support and without any obligation on the
*  part of SunSoft, Inc. to assist in its use, correction, modification or
*  enhancement.
*
*  SUNSOFT AND SUN MICROSYSTEMS, INC. SHALL HAVE NO LIABILITY WITH RESPECT
*  TO THE INFRINGEMENT OF COPYRIGHTS, TRADE SECRETS OR ANY PATENTS BY THIS
*  FILE OR ANY PART THEREOF.
*
*  IN NO EVENT WILL SUNSOFT OR SUN MICROSYSTEMS, INC. BE LIABLE FOR ANY
*  LOST REVENUE OR PROFITS OR OTHER SPECIAL, INDIRECT AND CONSEQUENTIAL
*  DAMAGES, EVEN IF THEY HAVE BEEN ADVISED OF THE POSSIBILITY OF SUCH
*  DAMAGES.
*
*  SunSoft, Inc.
*  2550 Garcia Avenue
*  Mountain View, California  94043
*/

int my_rw_init(my_rw_lock_t *rwp)
{
  pthread_condattr_t	cond_attr;

  pthread_mutex_init( &rwp->lock, MY_MUTEX_INIT_FAST);
  pthread_condattr_init( &cond_attr );
  pthread_cond_init( &rwp->readers, &cond_attr );
  pthread_cond_init( &rwp->writers, &cond_attr );
  pthread_condattr_destroy(&cond_attr);

  rwp->state	= 0;
  rwp->waiters	= 0;
#ifdef SAFE_MUTEX
  rwp->write_thread   = 0;
#endif

  return(0);
}


int my_rw_destroy(my_rw_lock_t *rwp)
{
  DBUG_ASSERT(rwp->state == 0);
  pthread_mutex_destroy( &rwp->lock );
  pthread_cond_destroy( &rwp->readers );
  pthread_cond_destroy( &rwp->writers );
  return(0);
}


int my_rw_rdlock(my_rw_lock_t *rwp)
{
  pthread_mutex_lock(&rwp->lock);

  /* active or queued writers */
  while (( rwp->state < 0 ) || rwp->waiters)
    pthread_cond_wait( &rwp->readers, &rwp->lock);

  rwp->state++;
  pthread_mutex_unlock(&rwp->lock);
  return(0);
}

int my_rw_tryrdlock(my_rw_lock_t *rwp)
{
  int res;
  pthread_mutex_lock(&rwp->lock);
  if ((rwp->state < 0 ) || rwp->waiters)
    res= EBUSY;					/* Can't get lock */
  else
  {
    res=0;
    rwp->state++;
  }
  pthread_mutex_unlock(&rwp->lock);
  return(res);
}


int my_rw_wrlock(my_rw_lock_t *rwp)
{
  pthread_mutex_lock(&rwp->lock);
  rwp->waiters++;				/* another writer queued */

  my_rw_lock_assert_not_write_owner(rwp);

  while (rwp->state)
    pthread_cond_wait(&rwp->writers, &rwp->lock);
  rwp->state	= -1;
  rwp->waiters--;
#ifdef SAFE_MUTEX
  rwp->write_thread= pthread_self();
#endif
  pthread_mutex_unlock(&rwp->lock);
  return(0);
}


int my_rw_trywrlock(my_rw_lock_t *rwp)
{
  int res;

  pthread_mutex_lock(&rwp->lock);
  if (rwp->state)
    res= EBUSY;					/* Can't get lock */    
  else
  {
    res=0;
    rwp->state	= -1;
#ifdef SAFE_MUTEX
  rwp->write_thread= pthread_self();
#endif
  }
  pthread_mutex_unlock(&rwp->lock);
  return(res);
}


int my_rw_unlock(my_rw_lock_t *rwp)
{
  DBUG_PRINT("rw_unlock",
	     ("state: %d waiters: %d", rwp->state, rwp->waiters));
  pthread_mutex_lock(&rwp->lock);

  DBUG_ASSERT(rwp->state != 0);

  if (rwp->state == -1)		/* writer releasing */
  {
    my_rw_lock_assert_write_owner(rwp);
    rwp->state= 0;		/* mark as available */
#ifdef SAFE_MUTEX
    rwp->write_thread= 0;
#endif

    if ( rwp->waiters )		/* writers queued */
      pthread_cond_signal( &rwp->writers );
    else
      pthread_cond_broadcast( &rwp->readers );
  }
  else
  {
    if ( --rwp->state == 0 &&   /* no more readers */
        rwp->waiters)
      pthread_cond_signal( &rwp->writers );
  }

  pthread_mutex_unlock( &rwp->lock );
  return(0);
}

#endif /* !defined _WIN32 */
#endif /* NEED_MY_RW_LOCK*/


int rw_pr_init(rw_pr_lock_t *rwlock)
{
  pthread_mutex_init(&rwlock->lock, NULL);
  pthread_cond_init(&rwlock->no_active_readers, NULL);
  rwlock->active_readers= 0;
  rwlock->writers_waiting_readers= 0;
  rwlock->active_writer= FALSE;
#ifdef SAFE_MUTEX
  rwlock->writer_thread= 0;
#endif
  return 0;
}


int rw_pr_destroy(rw_pr_lock_t *rwlock)
{
  pthread_cond_destroy(&rwlock->no_active_readers);
  pthread_mutex_destroy(&rwlock->lock);
  return 0;
}


int rw_pr_rdlock(rw_pr_lock_t *rwlock)
{
  pthread_mutex_lock(&rwlock->lock);
  /*
    The fact that we were able to acquire 'lock' mutex means
    that there are no active writers and we can acquire rd-lock.
    Increment active readers counter to prevent requests for
    wr-lock from succeeding and unlock mutex.
  */
  rwlock->active_readers++;
  pthread_mutex_unlock(&rwlock->lock);
  return 0;
}


int rw_pr_wrlock(rw_pr_lock_t *rwlock)
{
  pthread_mutex_lock(&rwlock->lock);

  if (rwlock->active_readers != 0)
  {
    /* There are active readers. We have to wait until they are gone. */
    rwlock->writers_waiting_readers++;

    while (rwlock->active_readers != 0)
      pthread_cond_wait(&rwlock->no_active_readers, &rwlock->lock);

    rwlock->writers_waiting_readers--;
  }

  /*
    We own 'lock' mutex so there is no active writers.
    Also there are no active readers.
    This means that we can grant wr-lock.
    Not releasing 'lock' mutex until unlock will block
    both requests for rd and wr-locks.
    Set 'active_writer' flag to simplify unlock.

    Thanks to the fact wr-lock/unlock in the absence of
    contention from readers is essentially mutex lock/unlock
    with a few simple checks make this rwlock implementation
    wr-lock optimized.
  */
  rwlock->active_writer= TRUE;
#ifdef SAFE_MUTEX
  rwlock->writer_thread= pthread_self();
#endif
  return 0;
}


int rw_pr_unlock(rw_pr_lock_t *rwlock)
{
  if (rwlock->active_writer)
  {
    /* We are unlocking wr-lock. */
#ifdef SAFE_MUTEX
    rwlock->writer_thread= 0;
#endif
    rwlock->active_writer= FALSE;
    if (rwlock->writers_waiting_readers)
    {
      /*
        Avoid expensive cond signal in case when there is no contention
        or it is wr-only.

        Note that from view point of performance it would be better to
        signal on the condition variable after unlocking mutex (as it
        reduces number of contex switches).

        Unfortunately this would mean that such rwlock can't be safely
        used by MDL subsystem, which relies on the fact that it is OK
        to destroy rwlock once it is in unlocked state.
      */
      pthread_cond_signal(&rwlock->no_active_readers);
    }
    pthread_mutex_unlock(&rwlock->lock);
  }
  else
  {
    /* We are unlocking rd-lock. */
    pthread_mutex_lock(&rwlock->lock);
    rwlock->active_readers--;
    if (rwlock->active_readers == 0 &&
        rwlock->writers_waiting_readers)
    {
      /*
        If we are last reader and there are waiting
        writers wake them up.
      */
      pthread_cond_signal(&rwlock->no_active_readers);
    }
    pthread_mutex_unlock(&rwlock->lock);
  }
  return 0;
}


