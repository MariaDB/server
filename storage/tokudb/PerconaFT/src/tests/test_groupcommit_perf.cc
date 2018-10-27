/* -*- mode: C++; c-basic-offset: 4; indent-tabs-mode: nil -*- */
// vim: ft=cpp:expandtab:ts=8:sw=4:softtabstop=4:
#ident "$Id$"
/*======
This file is part of PerconaFT.


Copyright (c) 2006, 2015, Percona and/or its affiliates. All rights reserved.

    PerconaFT is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License, version 2,
    as published by the Free Software Foundation.

    PerconaFT is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with PerconaFT.  If not, see <http://www.gnu.org/licenses/>.

----------------------------------------

    PerconaFT is free software: you can redistribute it and/or modify
    it under the terms of the GNU Affero General Public License, version 3,
    as published by the Free Software Foundation.

    PerconaFT is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU Affero General Public License for more details.

    You should have received a copy of the GNU Affero General Public License
    along with PerconaFT.  If not, see <http://www.gnu.org/licenses/>.
======= */

#ident "Copyright (c) 2006, 2015, Percona and/or its affiliates. All rights reserved."

#include "test.h"

/* Test using performance metrics only, to see if group commit is working. */

#include <db.h>
#include <toku_pthread.h>
#include <toku_time.h>
#include <sys/stat.h>

DB_ENV *env;
DB *db;

#define NITER 100

static void *
start_a_thread (void *i_p) {
    int *CAST_FROM_VOIDP(which_thread_p, i_p);
    int i,r;
    for (i=0; i<NITER; i++) {
	DB_TXN *tid;
	char keystr[100];
	DBT key,data;
	snprintf(keystr, sizeof(key), "%ld.%d.%d", random(), *which_thread_p, i);
	r=env->txn_begin(env, 0, &tid, 0); CKERR(r);
	r=db->put(db, tid,
		  dbt_init(&key, keystr, 1+strlen(keystr)),
		  dbt_init(&data, keystr, 1+strlen(keystr)),
		  0);
	r=tid->commit(tid, 0); CKERR(r);
    }
    return 0;
}

static void
test_groupcommit (int nthreads) {
    int r;
    DB_TXN *tid;

    r=db_env_create(&env, 0); assert(r==0);
    r=env->open(env, TOKU_TEST_FILENAME, DB_INIT_LOCK|DB_INIT_LOG|DB_INIT_MPOOL|DB_INIT_TXN|DB_CREATE|DB_PRIVATE|DB_THREAD, S_IRWXU+S_IRWXG+S_IRWXO); CKERR(r);
    r=db_create(&db, env, 0); CKERR(r);
    r=env->txn_begin(env, 0, &tid, 0); assert(r==0);
    r=db->open(db, tid, "foo.db", 0, DB_BTREE, DB_CREATE, S_IRWXU+S_IRWXG+S_IRWXO); CKERR(r);
    r=tid->commit(tid, 0);    assert(r==0);

    int i;
    toku_pthread_t threads[nthreads];
    int whichthread[nthreads];
    for (i = 0; i < nthreads; i++) {
        whichthread[i] = i;
        r = toku_pthread_create(toku_uninstrumented,
                                &threads[i],
                                nullptr,
                                start_a_thread,
                                &whichthread[i]);
    }
    for (i = 0; i < nthreads; i++) {
        toku_pthread_join(threads[i], 0);
    }
#if 0
    r=env->txn_begin(env, 0, &tid, 0); CKERR(r);
    char there[1000];
    memset(there, 'a',sizeof(there));
    there[999]=0;
    for (i=0; sum<(effective_max*3)/2; i++) {
	DBT key,data;
	char hello[20];
	snprintf(hello, 20, "hello%d", i);
	r=db->put(db, tid,
		  dbt_init(&key, hello, strlen(hello)+1),
		  dbt_init(&data, there, sizeof(there)),
		  0);
	assert(r==0);
	sum+=strlen(hello)+1+sizeof(there);
	if ((i+1)%10==0) {
	    r=tid->commit(tid, 0); assert(r==0);
	    r=env->txn_begin(env, 0, &tid, 0); CKERR(r);
	}
    }
    if (verbose) printf("i=%d sum=%d effmax=%d\n", i, sum, effective_max);
    r=tid->commit(tid, 0); assert(r==0);
#endif

    r=db->close(db, 0); assert(r==0);
    r=env->close(env, 0); assert(r==0);

}

static struct timeval prevtime;

static void
printtdiff (const char *str) {
    struct timeval thistime;
    gettimeofday(&thistime, 0);
    if (verbose) printf("%10.6f %s\n", toku_tdiff(&thistime, &prevtime), str);
    prevtime = thistime;
}

int
test_main (int argc, char *const argv[]) {
    parse_args(argc, argv);

    int r;
    toku_os_recursive_delete(TOKU_TEST_FILENAME);
    { r=toku_os_mkdir(TOKU_TEST_FILENAME, S_IRWXU+S_IRWXG+S_IRWXO);       assert(r==0); }

    gettimeofday(&prevtime, 0);
    test_groupcommit(1);  printtdiff("1 thread");
    test_groupcommit(2);  printtdiff("2 threads");
    test_groupcommit(10); printtdiff("10 threads");
    test_groupcommit(20); printtdiff("20 threads");
    return 0;
}
