/* -*- mode: C++; c-basic-offset: 4; indent-tabs-mode: nil -*- */
// vim: ft=cpp:expandtab:ts=8:sw=4:softtabstop=4:
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

#ifndef _TOKUDB_BACKGROUND_H
#define _TOKUDB_BACKGROUND_H

#include "hatoku_hton.h"
#include <atomic>
#include <list>

namespace tokudb {
namespace background {

class job_manager_t {
public:
    class job_t {
    public:
        // NO default constructor
        job_t() = delete;

        job_t(bool user_scheduled);

        virtual ~job_t();

        // method that runs the job
        inline void run();

        // method that tells the job to cancel ASAP
        inline void cancel();

        // method that tells the job to clean up/free resources on cancel
        // or completion
        inline void destroy();

        // method that returns a 'key' string for finding a specific job
        // (or jobs) usually used to find jobs to cancel
        virtual const char* key() = 0;

        // method to obtain the database name the job is scheduled on
        virtual const char* database() = 0;

        // method to obtain the table name the job is scheduled on
        virtual const char* table() = 0;

        // method to obtain the type of job
        virtual const char* type() = 0;

        // method to obtain a stringized list of job parameters
        virtual const char* parameters() = 0;

        // method to obtain a sting identifying the current status of the job
        virtual const char* status() = 0;

        inline bool running() const;

        inline bool cancelled() const;

        inline uint64_t id() const;

        inline bool user_scheduled() const;

        inline time_t scheduled_time() const;

        inline time_t started_time() const;

    protected:
        // derived classed implement this to actually run their job
        virtual void on_run() {};

        // derived classes implement this to cancel their job
        virtual void on_cancel() {};

        // derived classes implement this to clean up/free resources
        virtual void on_destroy() {};

    private:
        static std::atomic<uint64_t> _next_id;
        std::atomic<bool>   _running;
        std::atomic<bool>   _cancelled;
        uint64_t            _id;
        bool                _user_scheduled;
        time_t              _scheduled_time;
        time_t              _started_time;
    };

    // pfn for iterate callback
    typedef void (*pfn_iterate_t)(class job_t*, void*);

public:
    void* operator new(size_t sz);
    void operator delete(void* p);

    job_manager_t();

    ~job_manager_t();

    // creates/initializes a singleton bjm
    void initialize();

    // destroys a bjm singleton
    // cancels all jobs abd frees all resources
    void destroy();

    // schedules or runs a job depending on the 'background' value
    // job specifics all depend on the implementation od 'job'
    // background jobs will be executed in a FIFO fashion
    // two jobs with the same key can not run concurrently
    // if a foreground job is attempted, any currently scheduled or running
    // background jobs will be cancelled first
    // if another foreground job is already running, a new foreground job with
    // the same key will be rejected
    bool run_job(job_t* newjob, bool background);

    // cancels any background job with a matching key
    bool cancel_job(const char* key);

    // iterates currently pending and running background jobs, calling
    // 'callback' with the 'extra' data provided and the original 'extra'
    // data passed when the job was scheduled
    void iterate_jobs(pfn_iterate_t callback, void* extra) const;

    // lock the bjm, this prevents anyone from running, cancelling or iterating
    // jobs in the bjm.
    inline void lock();
    inline void unlock();

private:
    static void* thread_func(void* v);

    void* real_thread_func();

    // _mutex MUST be held on entry, will release and reaquire on exit
    void run(job_t* job);

    // _mutex MUST be held on entry
    void cancel(job_t* job);
private:
    typedef std::list<job_t*> jobs_t;

    mutable tokudb::thread::mutex_t     _mutex;
    mutable tokudb::thread::semaphore_t _sem;
    mutable tokudb::thread::thread_t    _thread;
    jobs_t                              _background_jobs;
    jobs_t                              _foreground_jobs;
    std::atomic<bool>                   _shutdown;
};

extern job_manager_t*    _job_manager;

bool initialize();
bool destroy();

inline void job_manager_t::lock() {
    assert_debug(!_mutex.is_owned_by_me());
    mutex_t_lock(_mutex);
}
inline void job_manager_t::unlock() {
    assert_debug(_mutex.is_owned_by_me());
    mutex_t_unlock(_mutex);
}

inline void job_manager_t::job_t::run() {
    if (!_cancelled) {
        _running = true;
        _started_time = ::time(0);
        on_run();
        _running = false;
    }
}
inline void job_manager_t::job_t::cancel() {
    _cancelled = true;
    if (_running)
        on_cancel();
    while (_running) tokudb::time::sleep_microsec(500000);
    destroy();
}
void job_manager_t::job_t::destroy() {
    on_destroy();
}
inline bool job_manager_t::job_t::running() const {
    return _running;
}
inline bool job_manager_t::job_t::cancelled() const {
    return _cancelled;
}
inline uint64_t job_manager_t::job_t::id() const {
    return _id;
}
inline bool job_manager_t::job_t::user_scheduled() const {
    return _user_scheduled;
}
inline time_t job_manager_t::job_t::scheduled_time() const {
    return _scheduled_time;
}
inline time_t job_manager_t::job_t::started_time() const {
    return _started_time;
}
} // namespace background
} // namespace tokudb

#endif // _TOKUDB_BACKGROUND_H
