/* -*- mode: C++; c-basic-offset: 4; indent-tabs-mode: nil -*- */
// vim: ft=cpp:expandtab:ts=8:sw=4:softtabstop=4:

// test the race between start, release, and wait.  since start does not put
// its lock request into the pending set, the blocking txn could release its
// lock before the first txn waits.  this will block the first txn because its
// lock request is not known when the lock is released.  the bug fix is to try
// again when lock retries are locked out.

#include "lock_request.h"
#include <atomic>
#include <thread>
#include "locktree.h"
#include "locktree_unit_test.h"
#include "test.h"

namespace toku {

    const uint64_t my_lock_wait_time = 1000 * 1000;  // ms
    const uint64_t my_killed_time = 1 * 1000;        // ms

    static uint64_t t_wait;

    static int my_killed_callback(void) {
        uint64_t t_now = toku_current_time_microsec();
        assert(t_now >= t_wait);
        if (t_now - t_wait >= my_killed_time * 1000)
            abort();
        return 0;
    }

    static void locktree_release_lock(locktree *lt,
                                      TXNID txn_id,
                                      const DBT *left,
                                      const DBT *right) {
        range_buffer buffer;
        buffer.create();
        buffer.append(left, right);
        lt->release_locks(txn_id, &buffer);
        buffer.destroy();
    }

    static void test_start_release_wait(void) {
        int r;

        locktree_manager mgr;
        mgr.create(nullptr, nullptr, nullptr, nullptr);

        DICTIONARY_ID dict_id = {1};
        locktree *lt = mgr.get_lt(dict_id, dbt_comparator, nullptr);

        const DBT *one = get_dbt(1);

        // a locks one
        lock_request a;
        a.create();
        a.set(lt, 1, one, one, lock_request::type::WRITE, false);
        r = a.start();
        assert(r == 0);

        // b tries to lock one, fails
        lock_request b;
        b.create();
        b.set(lt, 2, one, one, lock_request::type::WRITE, false);
        r = b.start();
        assert(r == DB_LOCK_NOTGRANTED);

        // a releases its lock
        locktree_release_lock(lt, 1, one, one);

        // b waits for one, gets locks immediately
        t_wait = toku_current_time_microsec();
        r = b.wait(my_lock_wait_time, my_killed_time, my_killed_callback);
        assert(r == 0);

        // b releases its lock so we can exit cleanly
        locktree_release_lock(lt, 2, one, one);

        a.destroy();
        b.destroy();

        mgr.release_lt(lt);
        mgr.destroy();
    }

} /* namespace toku */

int main(void) {
    toku::test_start_release_wait();
    return 0;
}
