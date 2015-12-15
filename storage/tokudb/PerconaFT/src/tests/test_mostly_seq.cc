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

#include <sys/stat.h>
#include <time.h>
#include <db.h>

static void
seqinsert (int n, float p) {
    if (verbose) printf("%s %d %f\n", __FUNCTION__, n, p);

    int r;
    toku_os_recursive_delete(TOKU_TEST_FILENAME);
    toku_os_mkdir(TOKU_TEST_FILENAME, S_IRWXU+S_IRWXG+S_IRWXO);

    DB_ENV *env;
    r = db_env_create(&env, 0); assert(r == 0);

    r = env->open(env, TOKU_TEST_FILENAME, DB_INIT_MPOOL + DB_PRIVATE + DB_CREATE, 077); assert(r == 0);

    DB *db;
    r = db_create(&db, env, 0); assert(r == 0);

    r = db->open(db, 0, "test.db", 0, DB_BTREE, DB_CREATE, S_IRWXU+S_IRWXG+S_IRWXO); assert(r == 0);

    int i;
    for (i = 2; i <= 2*n; i += 2) {
        int k = htonl(i);
        int v = i;
        DBT key, val;
        r = db->put(db, 0, dbt_init(&key, &k, sizeof k), dbt_init(&val, &v, sizeof v), 0); assert(r == 0);
        if (random() <= RAND_MAX * p) {
            k = htonl(i-1);
            v = i-1;
            r = db->put(db, 0, dbt_init(&key, &k, sizeof k), dbt_init(&val, &v, sizeof v), 0); assert(r == 0);
        }
    }

    r = db->close(db, 0); assert(r == 0);

    r = env->close(env, 0); assert(r == 0);
}

int
test_main(int argc, char *const argv[]) {
    srandom(time(0));
    int i;
    for (i=1; i<argc; i++) {
        const char *arg = argv[i];
        if (strcmp(arg, "-v") == 0) {
            verbose++;
            continue;
        }
        if (strcmp(arg, "-seed") == 0) {
            if (i+1 >= argc) return 1;
            srandom(atoi(argv[++i]));
            continue;
        }
    }

    int nodesize = 1024*1024;
    int entrysize = 25;
    int d = nodesize/entrysize;
    int n = d + d/4;

    float ps[] = { 0.0, 0.0001, 0.001, 0.01, 0.1, 0.25, 0.5, 1 };
    for (i=0; i<(int)(sizeof ps / sizeof (float)); i++) {
        seqinsert(n, ps[i]);
    }
    return 0;
}
