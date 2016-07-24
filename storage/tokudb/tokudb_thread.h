/* -*- mode: C++; c-basic-offset: 4; indent-tabs-mode: nil -*- */
// vim: ft=cpp:expandtab:ts=8:sw=4:softtabstop=4:
/* -*- mode: C; c-basic-offset: 4 -*- */
#ident "$Id$"
/*======
This file is part of TokuDB


Copyright (c) 2006, 2015, Percona and/or its affiliates. All rights reserved.

    TokuDBis is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License, version 2,
    as published by the Free Software Foundation.

    TokuDB is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with TokuDB.  If not, see <http://www.gnu.org/licenses/>.

======= */

#ident "Copyright (c) 2006, 2015, Percona and/or its affiliates. All rights reserved."

#ifndef _TOKUDB_SYNC_H
#define _TOKUDB_SYNC_H

#include "hatoku_defines.h"
#include "tokudb_debug.h"
#include "tokudb_time.h"

namespace tokudb {
namespace thread {

#if (defined(__MACH__) || defined(__APPLE__)) && _POSIX_TIMERS <= 0

#define _x_min(a, b) ((a) < (b) ? (a) : (b))

#define timed_lock_define(timed_func_name, lock_type_name, lock_func_name)     \
inline int timed_func_name(lock_type_name *mutex,                              \
                     const struct timespec *abs_timeout) {                     \
    int pthread_rc;                                                            \
    struct timespec remaining, slept, ts;                                      \
    static const int sleep_step = 1000000;                                     \
                                                                               \
    remaining = *abs_timeout;                                                  \
    while ((pthread_rc = lock_func_name(mutex)) == EBUSY) {                    \
        ts.tv_sec = 0;                                                         \
        ts.tv_nsec = (remaining.tv_sec > 0 ?                                   \
                      sleep_step :                                             \
                      _x_min(remaining.tv_nsec,sleep_step));                   \
        nanosleep(&ts, &slept);                                                \
        ts.tv_nsec -= slept.tv_nsec;                                           \
        if (ts.tv_nsec <= remaining.tv_nsec) {                                 \
            remaining.tv_nsec -= ts.tv_nsec;                                   \
        } else {                                                               \
            remaining.tv_sec--;                                                \
            remaining.tv_nsec =                                                \
              (sleep_step - (ts.tv_nsec - remaining.tv_nsec));                 \
        }                                                                      \
        if (remaining.tv_sec < 0 ||                                            \
            (!remaining.tv_sec && remaining.tv_nsec <= 0)) {                   \
            return ETIMEDOUT;                                                  \
        }                                                                      \
    }                                                                          \
                                                                               \
    return pthread_rc;                                                         \
}

timed_lock_define(pthread_mutex_timedlock,
                  pthread_mutex_t,
                  pthread_mutex_trylock);

timed_lock_define(pthread_rwlock_timedrdlock,
                  pthread_rwlock_t,
                  pthread_rwlock_tryrdlock);

timed_lock_define(pthread_rwlock_timedwrlock,
                  pthread_rwlock_t,
                  pthread_rwlock_trywrlock);

#endif //(defined(__MACH__) || defined(__APPLE__)) && _POSIX_TIMERS <= 0

uint my_tid(void);

// Your basic mutex
class mutex_t {
public:
    mutex_t(void);
    ~mutex_t(void);

    void lock(void);
    int lock(ulonglong microseconds);
    void unlock(void);
#ifdef TOKUDB_DEBUG
    bool is_owned_by_me(void) const;
#endif
private:
    static pthread_t _null_owner;
    pthread_mutex_t _mutex;
#ifdef TOKUDB_DEBUG
    uint            _owners;
    pthread_t       _owner;
#endif
};

// Simple read write lock
class rwlock_t {
public:
    rwlock_t(void);
    ~rwlock_t(void);

    void lock_read(void);
    int lock_read(ulonglong microseconds);
    void lock_write(void);
    int lock_write(ulonglong microseconds);
    void unlock(void);

private:
    rwlock_t(const rwlock_t&);
    rwlock_t& operator=(const rwlock_t&);

    pthread_rwlock_t	_rwlock;
};

// Simple event signal/wait class
class event_t {
public:
    // create_signalled - create the event in a signalled state
    // manual_reset - create an event that must be manually reset
    // after signaling
    event_t(
        bool create_signalled = false,
        bool manual_reset = false);
    ~event_t(void);

    // wait for the event to become signalled
    void wait(void);
    int wait(ulonglong microseconds);

    // signal the event
    void signal(void);

    // pulse the event (signal and free exactly one waiter)
    void pulse(void);

    // is the event currently signalled
    bool signalled(void);

    // unsignal/clear the event
    void reset(void);

private:
    event_t(const event_t&);
    event_t& operator=(const event_t&);

    pthread_mutex_t		_mutex;
    pthread_cond_t		_cond;
    bool			_signalled;
    bool			_pulsed;
    bool			_manual_reset;
};

// Semaphore signal/wait class
class semaphore_t {
public:
    // initial_count - the initial signal count of the semaphore
    // max_count - the maximum signal count for the semaphore.
    semaphore_t(int initial_count, int max_count);
    ~semaphore_t(void);

    enum E_WAIT {
        E_SIGNALLED = 0,
        E_INTERRUPTED = 1,
        E_TIMEDOUT = 2
    };

    // wait for the semaphore to become signalled
    E_WAIT wait(void);
    E_WAIT wait(ulonglong microseconds);

    // signal the semaphore to increase the count
    // return true if signalled, false if ignored due to count
    bool signal(void);

    // what is the semaphore signal count
    int signalled(void);

    // unsignal a signalled semaphore
    void reset(void);

    // set to interrupt any waiters, as long as is set,
    // waiters will return immediately with E_INTERRUPTED.
    // the semaphore signal count and tracking will continue
    // accepting signals and leave the signalled state intact
    void set_interrupt(void);
    void clear_interrupt(void);

private:
    semaphore_t(const semaphore_t&);
    semaphore_t& operator=(const semaphore_t&);

    pthread_mutex_t		_mutex;
    pthread_cond_t		_cond;
    bool			_interrupted;
    int			_signalled;
    int			_initial_count;
    int			_max_count;
};

// Thread class
class thread_t {
public:
    thread_t(void);
    ~thread_t(void);

    int start(void* (*pfn)(void*), void* arg);
    int join(void** value_ptr);
    int detach(void);

private:
    pthread_t   _thread;
};


inline uint my_tid(void) {
    return (uint)toku_os_gettid();
}


inline mutex_t::mutex_t(void) {
    #ifdef TOKUDB_DEBUG
        _owners = 0;
        _owner = _null_owner;
    #endif
    int r = pthread_mutex_init(&_mutex, MY_MUTEX_INIT_FAST);
    assert_debug(r == 0);
}
inline mutex_t::~mutex_t(void) {
    #ifdef TOKUDB_DEBUG
        assert_debug(_owners == 0);
    #endif
    int r = pthread_mutex_destroy(&_mutex);
    assert_debug(r == 0);
}
inline void mutex_t::lock(void) {
    assert_debug(is_owned_by_me() == false);
    int r = pthread_mutex_lock(&_mutex);
    assert_debug(r == 0);
    #ifdef TOKUDB_DEBUG
        _owners++;
        _owner = pthread_self();
    #endif
}
inline int mutex_t::lock(ulonglong microseconds) {
    assert_debug(is_owned_by_me() == false);
    timespec waittime = time::offset_timespec(microseconds);
    int r = pthread_mutex_timedlock(&_mutex, &waittime);
    #ifdef TOKUDB_DEBUG
        if (r == 0) {
            _owners++;
            _owner = pthread_self();
        }
    #endif
    assert_debug(r == 0 || r == ETIMEDOUT);
    return r;
}
inline void mutex_t::unlock(void) {
    #ifdef TOKUDB_DEBUG
        assert_debug(_owners > 0);
        assert_debug(is_owned_by_me());
        _owners--;
        _owner = _null_owner;
    #endif
    int r = pthread_mutex_unlock(&_mutex);
    assert_debug(r == 0);
}
#ifdef TOKUDB_DEBUG
inline bool mutex_t::is_owned_by_me(void) const {
    return pthread_equal(pthread_self(), _owner) != 0 ? true : false;
}
#endif


inline rwlock_t::rwlock_t(void) {
    int r = pthread_rwlock_init(&_rwlock, NULL);
    assert_debug(r == 0);
}
inline rwlock_t::~rwlock_t(void) {
    int r = pthread_rwlock_destroy(&_rwlock);
    assert_debug(r == 0);
}
inline void rwlock_t::lock_read(void) {
    int r;
    while ((r = pthread_rwlock_rdlock(&_rwlock)) != 0) {
        if (r == EBUSY || r == EAGAIN) {
            time::sleep_microsec(1000);
            continue;
        }
        break;
    }
    assert_debug(r == 0);
}
inline int rwlock_t::lock_read(ulonglong microseconds) {
    int r;
    timespec waittime = time::offset_timespec(microseconds);
    while ((r = pthread_rwlock_timedrdlock(&_rwlock, &waittime)) != 0) {
        if (r == EBUSY || r == EAGAIN) {
            time::sleep_microsec(1000);
            continue;
        } else if (r == ETIMEDOUT) {
            return ETIMEDOUT;
        }
        break;
    }
    assert_debug(r == 0);
    return r;
}
inline void rwlock_t::lock_write(void) {
    int r;
    while ((r = pthread_rwlock_wrlock(&_rwlock)) != 0) {
        if (r == EBUSY || r == EAGAIN) {
            time::sleep_microsec(1000);
            continue;
        }
        break;
    }
    assert_debug(r == 0);
}
inline int rwlock_t::lock_write(ulonglong microseconds) {
    int r;
    timespec waittime = time::offset_timespec(microseconds);
    while ((r = pthread_rwlock_timedwrlock(&_rwlock, &waittime)) != 0) {
        if (r == EBUSY || r == EAGAIN) {
            time::sleep_microsec(1000);
            continue;
        } else if (r == ETIMEDOUT) {
            return ETIMEDOUT;
        }
        break;
    }
    assert_debug(r == 0);
    return r;
}
inline void rwlock_t::unlock(void) {
    int r = pthread_rwlock_unlock(&_rwlock);
    assert_debug(r == 0);
}
inline rwlock_t::rwlock_t(const rwlock_t&) {
}
inline rwlock_t& rwlock_t::operator=(const rwlock_t&) {
    return *this;
}


inline event_t::event_t(bool create_signalled, bool manual_reset) :
    _manual_reset(manual_reset) {

    int r = pthread_mutex_init(&_mutex, NULL);
    assert_debug(r == 0);
    r = pthread_cond_init(&_cond, NULL);
    assert_debug(r == 0);
    if (create_signalled) {
        _signalled = true;
    } else {
        _signalled = false;
    }
    _pulsed = false;
}
inline event_t::~event_t(void) {
    int r = pthread_mutex_destroy(&_mutex);
    assert_debug(r == 0);
    r = pthread_cond_destroy(&_cond);
    assert_debug(r == 0);
}
inline void event_t::wait(void) {
    int r = pthread_mutex_lock(&_mutex);
    assert_debug(r == 0);
    while (_signalled == false && _pulsed == false) {
        r = pthread_cond_wait(&_cond, &_mutex);
        assert_debug(r == 0);
    }
    if (_manual_reset == false)
        _signalled = false;
    if (_pulsed)
        _pulsed = false;
    r = pthread_mutex_unlock(&_mutex);
    assert_debug(r == 0);
    return;
}
inline int event_t::wait(ulonglong microseconds) {
    timespec waittime = time::offset_timespec(microseconds);
    int r = pthread_mutex_timedlock(&_mutex, &waittime);
    if (r == ETIMEDOUT) return ETIMEDOUT;
    assert_debug(r == 0);
    while (_signalled == false && _pulsed == false) {
        r = pthread_cond_timedwait(&_cond, &_mutex, &waittime);
        if (r == ETIMEDOUT) {
            r = pthread_mutex_unlock(&_mutex);
            assert_debug(r == 0);
            return ETIMEDOUT;
        }
        assert_debug(r == 0);
    }
    if (_manual_reset == false)
        _signalled = false;
    if (_pulsed)
        _pulsed = false;
    r = pthread_mutex_unlock(&_mutex);
    assert_debug(r == 0);
    return 0;
}
inline void event_t::signal(void) {
    int r = pthread_mutex_lock(&_mutex);
    assert_debug(r == 0);
    _signalled = true;
    if (_manual_reset) {
        r = pthread_cond_broadcast(&_cond);
        assert_debug(r == 0);
    } else {
        r = pthread_cond_signal(&_cond);
        assert_debug(r == 0);
    }
    r = pthread_mutex_unlock(&_mutex);
    assert_debug(r == 0);
}
inline void event_t::pulse(void) {
    int r = pthread_mutex_lock(&_mutex);
    assert_debug(r == 0);
    _pulsed = true;
    r = pthread_cond_signal(&_cond);
    assert_debug(r == 0);
    r = pthread_mutex_unlock(&_mutex);
    assert_debug(r == 0);
}
inline bool event_t::signalled(void) {
    bool ret = false;
    int r = pthread_mutex_lock(&_mutex);
    assert_debug(r == 0);
    ret = _signalled;
    r = pthread_mutex_unlock(&_mutex);
    assert_debug(r == 0);
    return ret;
}
inline void event_t::reset(void) {
    int r = pthread_mutex_lock(&_mutex);
    assert_debug(r == 0);
    _signalled = false;
    _pulsed = false;
    r = pthread_mutex_unlock(&_mutex);
    assert_debug(r == 0);
    return;
}
inline event_t::event_t(const event_t&) {
}
inline event_t& event_t::operator=(const event_t&) {
    return *this;
}


inline semaphore_t::semaphore_t(
    int initial_count,
    int max_count) :
    _interrupted(false),
    _initial_count(initial_count),
    _max_count(max_count) {

    int r = pthread_mutex_init(&_mutex, NULL);
    assert_debug(r == 0);
    r = pthread_cond_init(&_cond, NULL);
    assert_debug(r == 0);
    _signalled = _initial_count;
}
inline semaphore_t::~semaphore_t(void) {
    int r = pthread_mutex_destroy(&_mutex);
    assert_debug(r == 0);
    r = pthread_cond_destroy(&_cond);
    assert_debug(r == 0);
}
inline semaphore_t::E_WAIT semaphore_t::wait(void) {
    E_WAIT ret;
    int r = pthread_mutex_lock(&_mutex);
    assert_debug(r == 0);
    while (_signalled == 0 && _interrupted == false) {
        r = pthread_cond_wait(&_cond, &_mutex);
        assert_debug(r == 0);
    }
    if (_interrupted) {
        ret = E_INTERRUPTED;
    } else {
        _signalled--;
        ret = E_SIGNALLED;
    }
    r = pthread_mutex_unlock(&_mutex);
    assert_debug(r == 0);
    return ret;
}
inline semaphore_t::E_WAIT semaphore_t::wait(ulonglong microseconds) {
    E_WAIT ret;
    timespec waittime = time::offset_timespec(microseconds);
    int r = pthread_mutex_timedlock(&_mutex, &waittime);
    if (r == ETIMEDOUT) return E_TIMEDOUT;
    assert_debug(r == 0);
    while (_signalled == 0 && _interrupted == false) {
        r = pthread_cond_timedwait(&_cond, &_mutex, &waittime);
        if (r == ETIMEDOUT) {
            r = pthread_mutex_unlock(&_mutex);
            assert_debug(r == 0);
            return E_TIMEDOUT;
        }
        assert_debug(r == 0);
    }
    if (_interrupted) {
        ret = E_INTERRUPTED;
    } else {
        _signalled--;
        ret = E_SIGNALLED;
    }
    r = pthread_mutex_unlock(&_mutex);
    assert_debug(r == 0);
    return ret;
}
inline bool semaphore_t::signal(void) {
    bool ret = false;
    int r = pthread_mutex_lock(&_mutex);
    assert_debug(r == 0);
    if (_signalled < _max_count) {
        _signalled++;
        ret = true;
    }
    r = pthread_cond_signal(&_cond);
    assert_debug(r == 0);
    r = pthread_mutex_unlock(&_mutex);
    assert_debug(r == 0);
    return ret;
}
inline int semaphore_t::signalled(void) {
    int ret = 0;
    int r = pthread_mutex_lock(&_mutex);
    assert_debug(r == 0);
    ret = _signalled;
    r = pthread_mutex_unlock(&_mutex);
    assert_debug(r == 0);
    return ret;
}
inline void semaphore_t::reset(void) {
    int r = pthread_mutex_lock(&_mutex);
    assert_debug(r == 0);
    _signalled = 0;
    r = pthread_mutex_unlock(&_mutex);
    assert_debug(r == 0);
    return;
}
inline void semaphore_t::set_interrupt(void) {
    int r = pthread_mutex_lock(&_mutex);
    assert_debug(r == 0);
    _interrupted = true;
    r = pthread_cond_broadcast(&_cond);
    assert_debug(r == 0);
    r = pthread_mutex_unlock(&_mutex);
    assert_debug(r == 0);
}
inline void semaphore_t::clear_interrupt(void) {
    int r = pthread_mutex_lock(&_mutex);
    assert_debug(r == 0);
    _interrupted = false;
    r = pthread_mutex_unlock(&_mutex);
    assert_debug(r == 0);
}
inline semaphore_t::semaphore_t(const semaphore_t&) {
}
inline semaphore_t& semaphore_t::operator=(const semaphore_t&) {
    return *this;
}


inline thread_t::thread_t(void) : _thread(0) {
}
inline thread_t::~thread_t(void) {
}
inline int thread_t::start(void*(*pfn)(void*), void* arg) {
    return pthread_create(&_thread, NULL, pfn, arg);
}
inline int thread_t::join(void** value_ptr) {
    return pthread_join(_thread, value_ptr);
}
inline int thread_t::detach(void) {
    return pthread_detach(_thread);
}

} // namespace thread
} // namespace tokudb


#endif // _TOKUDB_SYNC_H
