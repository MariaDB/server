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
#include <db.h>
#include <toku_portability.h>
#include <toku_os.h>
#include <memory.h>
#include <stdint.h>
#include <stdlib.h>

DB_TXN * const null_txn = nullptr;

const size_t nodesize = 128 << 10;
const size_t keysize = 8;
const size_t valsize = 92;
const size_t rowsize = keysize + valsize;
const int max_degree = 16;
const size_t numleaves = max_degree * 3; // want height 2, this should be good enough
const size_t numrows = (numleaves * nodesize + rowsize) / rowsize;

static void test_seqinsert(bool asc) {
    int r;
    toku_os_recursive_delete(TOKU_TEST_FILENAME);
    r = toku_os_mkdir(TOKU_TEST_FILENAME, S_IRWXU+S_IRWXG+S_IRWXO);
    CKERR(r);

    DB_ENV *env;
    r = db_env_create(&env, 0);
    CKERR(r);
    r = env->open(env, TOKU_TEST_FILENAME, DB_INIT_LOCK|DB_INIT_MPOOL|DB_INIT_TXN|DB_CREATE|DB_PRIVATE, S_IRWXU+S_IRWXG+S_IRWXO);
    CKERR(r);

    DB *db;
    r = db_create(&db, env, 0);
    CKERR(r);
    r = db->set_pagesize(db, nodesize);
    CKERR(r);
    r = db->open(db, null_txn, "seqinsert", NULL, DB_BTREE, DB_CREATE, 0666);
    CKERR(r);

    {
        DB_TXN *txn;
        r = env->txn_begin(env, 0, &txn, 0);
        CKERR(r);

        char v[valsize];
        ZERO_ARRAY(v);
        uint64_t k;
        DBT key, val;
        dbt_init(&key, &k, sizeof k);
        dbt_init(&val, v, valsize);
        for (size_t i = 0; i < numrows; ++i) {
            k = toku_htod64(numrows + (asc ? i : -i));
            r = db->put(db, txn, &key, &val, 0);
            CKERR(r);
        }

        r = txn->commit(txn, 0);
        CKERR(r);
    }

    r = db->close(db, 0);
    CKERR(r);

    r = env->close(env, 0);
    CKERR(r);
}

int test_main(int argc, char * const argv[]) {
    default_parse_args(argc, argv);

    test_seqinsert(true);
    test_seqinsert(false);

    return 0;
}
