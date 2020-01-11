/* -*- mode: C++; c-basic-offset: 4; indent-tabs-mode: nil -*- */
// vim: ft=cpp:expandtab:ts=8:sw=4:softtabstop=4:
/* -*- mode: C; c-basic-offset: 4 -*- */
#ident "$Id$"
/*======
This file is part of TokuDB


Copyright (c) 2006, 2015, Percona and/or its affiliates. All rights reserved.

    TokuDB is free software: you can redistribute it and/or modify
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

#include "tokudb_background.h"
#include "tokudb_sysvars.h"

namespace tokudb {
namespace background {


std::atomic<uint64_t> job_manager_t::job_t::_next_id(1);

job_manager_t::job_t::job_t(bool user_scheduled) :
    _running(false),
    _cancelled(false),
    _id(_next_id++),
    _user_scheduled(user_scheduled),
    _scheduled_time(::time(0)),
    _started_time(0) {
}
job_manager_t::job_t::~job_t() {
}
void* job_manager_t::operator new(size_t sz) {
    return tokudb::memory::malloc(sz, MYF(MY_WME|MY_ZEROFILL|MY_FAE));
}
void job_manager_t::operator delete(void* p) {
    tokudb::memory::free(p);
}
job_manager_t::job_manager_t() :
    _sem(0, 65535),
    _shutdown(false) {
}
job_manager_t::~job_manager_t() {
}
void job_manager_t::initialize() {
    int r = _thread.start(thread_func, this);
    assert_always(r == 0);
}
void job_manager_t::destroy() {
    assert_always(!_shutdown);
    assert_always(_foreground_jobs.size() == 0);
    _shutdown = true;
    _sem.set_interrupt();

    while (_background_jobs.size()) {
        mutex_t_lock(_mutex);
        job_t* job = _background_jobs.front();
        if (!job->cancelled())
            cancel(job);
        _background_jobs.pop_front();
        delete job;
        mutex_t_unlock(_mutex);
    }

    void* result;
    int r = _thread.join(&result);
    assert_always(r == 0);
}
bool job_manager_t::run_job(job_t* newjob, bool background) {
    bool ret = false;
    const char* jobkey = newjob->key();

    mutex_t_lock(_mutex);
    assert_always(!_shutdown);

    for (jobs_t::iterator it = _background_jobs.begin();
         it != _background_jobs.end();
         it++) {
        job_t* job = *it;
        if (!job->cancelled() && strcmp(job->key(), jobkey) == 0) {
            // if this is a foreground job being run and
            // there is an existing background job of the same type
            // and it is not running yet, we can cancel the background job
            // and just run this one in the foreground, might have different
            // params, but that is up to the user to figure out.
            if (!background && !job->running()) {
                job->cancel();
            } else {
                // can't schedule or run another job on the same key
                goto cleanup;
            }
        }
    }
    for (jobs_t::iterator it = _foreground_jobs.begin();
         it != _foreground_jobs.end();
         it++) {
        job_t* job = *it;
        if (strcmp(job->key(), jobkey) == 0) {
            // can't schedule or run another job on the same key
            // as an existing foreground job
            goto cleanup;
        }
    }

    if (background) {
        _background_jobs.push_back(newjob);
        _sem.signal();
        ret = true;
    } else {
        _foreground_jobs.push_back(newjob);

        run(newjob);

        for (jobs_t::iterator it = _foreground_jobs.begin();
             it != _foreground_jobs.end();
             it++) {
            job_t* job = *it;
            if (job == newjob) {
                _foreground_jobs.erase(it);
                delete job;
                break;
            }
        }
        ret = true;
    }

cleanup:
    mutex_t_unlock(_mutex);
    return ret;
}
bool job_manager_t::cancel_job(const char* key) {
    bool ret = false;
    mutex_t_lock(_mutex);

    for (jobs_t::iterator it = _background_jobs.begin();
         it != _background_jobs.end();
         it++) {
        job_t* job = *it;

        if (!job->cancelled() && strcmp(job->key(), key) == 0) {
            cancel(job);
            ret = true;
        }
    }

    mutex_t_unlock(_mutex);
    return ret;
}
void job_manager_t::iterate_jobs(pfn_iterate_t callback, void* extra) const {
    mutex_t_lock(_mutex);

    for (jobs_t::const_iterator it = _background_jobs.begin();
         it != _background_jobs.end();
         it++) {
        job_t* job = *it;
        if (!job->cancelled()) {
            callback(job, extra);
        }
    }

    mutex_t_unlock(_mutex);
}
void* job_manager_t::thread_func(void* v) {
    return ((tokudb::background::job_manager_t*)v)->real_thread_func();
}
void* job_manager_t::real_thread_func() {
    while (_shutdown == false) {
        tokudb::thread::semaphore_t::E_WAIT res = _sem.wait();
        if (res == tokudb::thread::semaphore_t::E_INTERRUPTED || _shutdown) {
                break;
        } else if (res == tokudb::thread::semaphore_t::E_SIGNALLED) {
#if defined(TOKUDB_DEBUG)
            if (TOKUDB_UNLIKELY(
                    tokudb::sysvars::debug_pause_background_job_manager)) {
                _sem.signal();
                tokudb::time::sleep_microsec(250000);
                continue;
            }
#endif  // defined(TOKUDB_DEBUG)

            mutex_t_lock(_mutex);
            assert_debug(_background_jobs.size() > 0);
            job_t* job = _background_jobs.front();
            run(job);
            _background_jobs.pop_front();
            mutex_t_unlock(_mutex);
            delete job;
        }
    }
    return NULL;
}
void job_manager_t::run(job_t* job) {
    assert_debug(_mutex.is_owned_by_me());
    if (!job->cancelled()) {
        mutex_t_unlock(_mutex);
        // do job
        job->run();
        // done job
        mutex_t_lock(_mutex);
    }
    if (!job->cancelled()) {
        job->destroy();
    }
}
void job_manager_t::cancel(job_t* job) {
    assert_debug(_mutex.is_owned_by_me());
    assert_always(!job->cancelled());
    job->cancel();
}
job_manager_t* _job_manager = NULL;

bool initialize() {
    assert_always(_job_manager == NULL);
    _job_manager = new job_manager_t;
    _job_manager->initialize();
    return true;
}
bool destroy() {
    _job_manager->destroy();
    delete _job_manager;
    _job_manager = NULL;
    return true;
}
} // namespace background
} // namespace tokudb
