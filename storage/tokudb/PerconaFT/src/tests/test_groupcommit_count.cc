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

/* Test by counting the fsyncs, to see if group commit is working. */

#include <db.h>
#include <toku_pthread.h>
#include <toku_time.h>
#include <sys/stat.h>
#include <unistd.h>

DB_ENV *env;
DB *db;
int do_sync=1;

#define NITER 100

static void *start_a_thread (void *i_p) {
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
	r=tid->commit(tid, do_sync ? 0 : DB_TXN_NOSYNC); CKERR(r);
    }
    return 0;
}

const char *env_path;

static void
test_groupcommit (int nthreads) {
    int r;
    DB_TXN *tid;

    r=db_env_create(&env, 0); assert(r==0);
    r=env->open(env, env_path, DB_INIT_LOCK|DB_INIT_LOG|DB_INIT_MPOOL|DB_INIT_TXN|DB_CREATE|DB_PRIVATE|DB_THREAD, S_IRWXU+S_IRWXG+S_IRWXO); CKERR(r);
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

    r=db->close(db, 0); assert(r==0);
    r=env->close(env, 0); assert(r==0);

    //if (verbose) printf(" That's a total of %d commits\n", nthreads*NITER);
}

// helgrind doesn't understand that pthread_join removes a race condition.   I'm not impressed... -Bradley
// Also, it doesn't happen every time, making helgrind unsuitable for regression tests.
// So we must put locks around things that are properly serialized anyway.

static int fsync_count_maybe_lockprotected=0;
static void
inc_fsync_count (void) {
    fsync_count_maybe_lockprotected++;
}

static int
get_fsync_count (void) {
    int result=fsync_count_maybe_lockprotected;
    return result;
}

static int
do_fsync (int fd) {
    //fprintf(stderr, "%8.6fs Thread %ld start fsyncing\n", get_tdiff(), pthread_self());
    inc_fsync_count();
    int r = fsync(fd);
    //fprintf(stderr, "%8.6fs Thread %ld done  fsyncing\n", get_tdiff(), pthread_self());
    return r;
}

static const char *progname;
static struct timeval prevtime;
static int prev_count;

static void
printtdiff (int N) {
    struct timeval thistime;
    gettimeofday(&thistime, 0);
    double diff = toku_tdiff(&thistime, &prevtime);
    int fcount=get_fsync_count();
    if (verbose) printf("%s: %10.6fs %4d fsyncs for %4d threads %s %8.1f tps, %8.1f tps/thread\n", progname, diff, fcount-prev_count,
			N,
			do_sync ? "with sync         " : "with DB_TXN_NOSYNC",
			NITER*(N/diff), NITER/diff);
    prevtime=thistime;
    prev_count=fcount;
}

static void
do_test (int N) {
    for (do_sync = 0; do_sync<2; do_sync++) {
	int count_before = get_fsync_count();
	test_groupcommit(N);
	printtdiff(N);
	int count_after = get_fsync_count();
	if (count_after-count_before >= N*NITER) {
	    if (verbose) printf("It looks like too many fsyncs.  Group commit doesn't appear to be occuring. %d - %d >= %d\n", count_after, count_before, N*NITER);
	    exit(1);
	}
    }
}

int log_max_n_threads_over_10 = 3;

static void
my_parse_args (int argc, char *const argv[]) {
    verbose=1; // use -q to turn off the talking.
    env_path = TOKU_TEST_FILENAME;
    const char *argv0=argv[0];
    while (argc>1) {
	int resultcode=0;
	if (strcmp(argv[1], "-v")==0) {
	    verbose++;
	} else if (strcmp(argv[1],"-q")==0) {
	    verbose--;
	    if (verbose<0) verbose=0;
	} else if (strcmp(argv[1],"-n")==0) {
	    argc--;
	    argv++;
	    if (argc<=1) { resultcode=1; goto do_usage; }
	    errno = 0;
	    char *end;
	    log_max_n_threads_over_10 = strtol(argv[1], &end, 10);
	    if (errno!=0 || *end) {
		resultcode=1;
		goto do_usage;
	    }
	} else if (strcmp(argv[1], "-h")==0) {
	do_usage:
	    fprintf(stderr, "Usage:\n%s [-v|-q] [-n LOG(MAX_N_THREADS/10)] [-h]\n", argv0);
	    exit(resultcode);
	} else {
	    resultcode=1;
	    goto do_usage;
	}
	argc--;
	argv++;
    }
}


int
test_main (int argc, char *const argv[]) {
    progname=argv[0];
    my_parse_args(argc, argv);

    gettimeofday(&prevtime, 0);
    prev_count=0;

    db_env_set_func_fsync(do_fsync);
    db_env_set_num_bucket_mutexes(32);

    toku_os_recursive_delete(env_path);
    { int r=toku_os_mkdir(env_path, S_IRWXU+S_IRWXG+S_IRWXO);       assert(r==0); }

    test_groupcommit(1);  printtdiff(1);
    test_groupcommit(2);  printtdiff(2);
    for (int i=0; i<log_max_n_threads_over_10; i++) {
	do_test(10 << i);
    }
    return 0;
}
