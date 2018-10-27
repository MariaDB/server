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
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include <errno.h>
#include <sys/stat.h>
#include <db.h>
#include <toku_pthread.h>

static inline unsigned int getmyid(void) {
    return toku_os_gettid();
}

typedef unsigned int my_t;

struct db_inserter {
    toku_pthread_t tid;
    DB *db;
    my_t startno, endno;
    int do_exit;
};

static int
db_put (DB *db, my_t k, my_t v) {
    DBT key, val;
    int r = db->put(db, 0, dbt_init(&key, &k, sizeof k), dbt_init(&val, &v, sizeof v), 0);
    return r;
}

static void *
do_inserts (void *arg) {
    struct db_inserter *mywork = (struct db_inserter *) arg;
    if (verbose) {
        toku_pthread_t self = toku_pthread_self();
        printf("%lu:%u:do_inserts:start:%u-%u\n", *(unsigned long*)&self, getmyid(), mywork->startno, mywork->endno);
    }
    my_t i;
    for (i=mywork->startno; i < mywork->endno; i++) {
        int r = db_put(mywork->db, htonl(i), i); assert(r == 0);
    }
    
    if (verbose) {
        toku_pthread_t self = toku_pthread_self();
        printf("%lu:%u:do_inserts:end\n", *(unsigned long*)&self, getmyid());
    }
    // Don't call toku_pthread_exit(), since it has a memory leak.
    // if (mywork->do_exit) toku_pthread_exit(arg);
    return 0;
}

static int
usage (void) {
    fprintf(stderr, "test [-n NTUPLES] [-p NTHREADS]\n");
    fprintf(stderr, "default NTUPLES=1000000\n");
    fprintf(stderr, "default NTHREADS=2\n");
    return 1;
}

int
test_main(int argc, char *const argv[]) {
    const char *dbfile = "test.db";
    const char *dbname = "main";
    int nthreads = 2;
    my_t n = 1000000;

    int r;
    toku_os_recursive_delete(TOKU_TEST_FILENAME);
    toku_os_mkdir(TOKU_TEST_FILENAME, S_IRWXU+S_IRWXG+S_IRWXO);

    int i;
    for (i=1; i<argc; i++) {
        const char *arg = argv[i];
        if (0 == strcmp(arg, "-h") || 0 == strcmp(arg, "--help")) {
            return usage();
        }
        if (0 == strcmp(arg, "-v") || 0 == strcmp(arg, "--verbose")) {
            verbose = 1; 
            continue;
        }
        if (0 == strcmp(arg, "-p")) {
            if (i+1 >= argc) return usage();
            nthreads = atoi(argv[++i]);
            continue;
        }
        if (0 == strcmp(arg, "-n")) {
            if (i+1 >= argc) return usage();
            n = atoi(argv[++i]);
            continue;
        }
    }

    DB_ENV *env;

    r = db_env_create(&env, 0); assert(r == 0);
    r = env->set_cachesize(env, 0, 128000000, 1); assert(r == 0);
    r = env->open(env, TOKU_TEST_FILENAME, DB_CREATE + DB_THREAD + DB_PRIVATE + DB_INIT_MPOOL + DB_INIT_LOCK, S_IRWXU+S_IRWXG+S_IRWXO); assert(r == 0);

    DB *db;

    r = db_create(&db, env, 0); assert(r == 0);
    r = db->open(db, 0, dbfile, dbname, DB_BTREE, DB_CREATE + DB_THREAD, S_IRWXU+S_IRWXG+S_IRWXO); assert(r == 0);

    struct db_inserter work[nthreads];

    for (i=0; i<nthreads; i++) {
        work[i].db = db;
        work[i].startno = i*(n/nthreads);
        work[i].endno = work[i].startno + (n/nthreads);
        work[i].do_exit =1 ;
        if (i+1 == nthreads)  
            work[i].endno = n;
    }

    if (verbose)
        printf("pid:%d\n", toku_os_getpid());

    for (i = 1; i < nthreads; i++) {
        r = toku_pthread_create(
            toku_uninstrumented, &work[i].tid, nullptr, do_inserts, &work[i]);
        assert(r == 0);
    }

    work[0].do_exit = 0;
    do_inserts(&work[0]);

    for (i=1; i<nthreads; i++) {
        void *ret;
        r = toku_pthread_join(work[i].tid, &ret); assert(r == 0);
    }

    r = db->close(db, 0); assert(r == 0);
    r = env->close(env, 0); assert(r == 0);

    return 0;
}
