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
#include <vector>
#include <db.h>
#include "toku_time.h"

static void open_dbs(DB_ENV *env, int max_dbs) {
    std::vector<DB *> dbs;

    uint64_t t_start = toku_current_time_microsec();
    // open db's
    {
        uint64_t t0 = toku_current_time_microsec();
        for (int i = 1; i <= max_dbs; i++) {
            int r;
            DB *db = NULL;
            r = db_create(&db, env, 0);
            assert(r == 0);
            char db_name[32];
            sprintf(db_name, "db%d", i);
            r = db->open(db, NULL, db_name, NULL, DB_BTREE, DB_CREATE, 0666);
            assert(r == 0);
            dbs.push_back(db);
            if ((i % 100) == 0) {
                uint64_t t = toku_current_time_microsec();
                fprintf(stderr, "open %d %" PRIu64 "\n", i, t - t0);
                t0 = t;
            }
        }
    }
    uint64_t t_end = toku_current_time_microsec();
    fprintf(stderr, "%" PRIu64 "\n", t_end - t_start);

    // close db's
    {
        uint64_t t0 = toku_current_time_microsec();
        int i = 1;
        for (std::vector<DB *>::iterator dbi = dbs.begin(); dbi != dbs.end(); dbi++, i++) {
            DB *db = *dbi;
            int r = db->close(db, 0);
            assert(r == 0);
            if ((i % 100) == 0) {
                uint64_t t = toku_current_time_microsec();
                printf("close %d %" PRIu64 "\n", i, t - t0);
                t0 = t;
            }
        }
    }
}

int test_main (int argc, char * const argv[]) {
    int r;
    int max_dbs = 1;

    // parse_args(argc, argv);
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-v") == 0) {
            verbose++;
            continue;
        }
        if (strcmp(argv[i], "-q") == 0) {
            if (verbose > 0) verbose--;
            continue;
        }
        max_dbs = atoi(argv[i]);
        continue;
    }

    toku_os_recursive_delete(TOKU_TEST_FILENAME);
    r = toku_os_mkdir(TOKU_TEST_FILENAME, S_IRWXU+S_IRWXG+S_IRWXO);

    DB_ENV *env = NULL;
    r = db_env_create(&env, 0);
    assert(r == 0);
    env->set_errfile(env, stderr);
    r = env->open(env, TOKU_TEST_FILENAME, DB_INIT_LOCK+DB_INIT_MPOOL+DB_INIT_TXN+DB_INIT_LOG + DB_CREATE + DB_PRIVATE, S_IRWXU+S_IRWXG+S_IRWXO);
    assert(r == 0);

    open_dbs(env, max_dbs);

    r = env->close(env, 0);
    assert(r == 0);

    return 0;
}
