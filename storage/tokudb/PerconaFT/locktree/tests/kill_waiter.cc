/* -*- mode: C++; c-basic-offset: 4; indent-tabs-mode: nil -*- */
// vim: ft=cpp:expandtab:ts=8:sw=4:softtabstop=4:

// test the lock manager kill waiter function

#include "locktree.h"
#include "lock_request.h"
#include "test.h"
#include "locktree_unit_test.h"
#include <thread>
#include <atomic>

namespace toku {

const uint64_t my_lock_wait_time = 1000 * 1000;
const uint64_t my_killed_time = 500 * 1000;
const int n_locks = 4;

static int my_killed_callback(void) {
    if (1) fprintf(stderr, "%s:%u %s\n", __FILE__, __LINE__, __FUNCTION__);
    return 0;
}

static void locktree_release_lock(locktree *lt, TXNID txn_id, const DBT *left, const DBT *right) {
    range_buffer buffer;
    buffer.create();
    buffer.append(left, right);
    lt->release_locks(txn_id, &buffer);
    buffer.destroy();
}

static void wait_lock(lock_request *lr, std::atomic_int *done) {
    int r = lr->wait(my_lock_wait_time, my_killed_time, my_killed_callback);
    assert(r == DB_LOCK_NOTGRANTED);
    *done = 1;
}

static void test_kill_waiter(void) {
    int r;

    locktree_manager mgr;
    mgr.create(nullptr, nullptr, nullptr, nullptr);

    DICTIONARY_ID dict_id = { 1 };
    locktree *lt = mgr.get_lt(dict_id, dbt_comparator, nullptr);

    const DBT *one = get_dbt(1);

    lock_request locks[n_locks];
    std::thread waiters[n_locks-1];
    for (int i = 0; i < n_locks; i++) {
        locks[i].create();
        locks[i].set(lt, i+1, one, one, lock_request::type::WRITE, false, &waiters[i]);
    }

    // txn 'n_locks' grabs the lock
    r = locks[n_locks-1].start();
    assert_zero(r);

    for (int i = 0; i < n_locks-1; i++) {
        r = locks[i].start();
        assert(r == DB_LOCK_NOTGRANTED);
    }

    std::atomic_int done[n_locks-1];
    for (int i = 0; i < n_locks-1; i++) {
        done[i] = 0;
        waiters[i] = std::thread(wait_lock, &locks[i], &done[i]);
    }

    for (int i = 0; i < n_locks-1; i++) {
        assert(!done[i]);
    }

    sleep(1);
    for (int i = 0; i < n_locks-1; i++) {
        mgr.kill_waiter(&waiters[i]);
        while (!done[i]) sleep(1);
        waiters[i].join();
        for (int j = i+1; j < n_locks-1; j++)
            assert(!done[j]);
    }

    locktree_release_lock(lt, n_locks, one, one);

    for (int i = 0; i < n_locks; i++) {
        locks[i].destroy();
    }

    mgr.release_lt(lt);
    mgr.destroy();
}

} /* namespace toku */

int main(void) {
    toku::test_kill_waiter();
    return 0;
}

