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
#include <memory.h>
#include <errno.h>
#include <sys/stat.h>
#include <db.h>


static int put_multiple_generate(DB *UU(dest_db), DB *UU(src_db), DBT_ARRAY *dest_key_arrays, DBT_ARRAY *dest_val_arrays, const DBT *src_key, const DBT *src_val) {
    toku_dbt_array_resize(dest_key_arrays, 1);
    toku_dbt_array_resize(dest_val_arrays, 1);
    DBT *dest_key = &dest_key_arrays->dbts[0];
    DBT *dest_val = &dest_val_arrays->dbts[0];
    dest_key->flags = 0;
    dest_val->flags = 0;
    dbt_init(dest_key, src_key->data, src_key->size);
    dbt_init(dest_val, src_val->data, src_val->size);
    return 0;
}

static void
test_loader_abort (bool do_compress, bool abort_loader, bool abort_txn) {
    DB_ENV * env;
    DB *db;
    DB_TXN *txn;
    DB_TXN* const null_txn = 0;
    const char * const fname = "test.loader_abort.ft_handle";
    int r;
    toku_os_recursive_delete(TOKU_TEST_FILENAME);
    toku_os_mkdir(TOKU_TEST_FILENAME, S_IRWXU+S_IRWXG+S_IRWXO);

    DB_LOADER *loader;
    uint32_t db_flags = 0;
    uint32_t dbt_flags = 0;    
    uint32_t loader_flags = do_compress ? LOADER_COMPRESS_INTERMEDIATES : 0;
    DBC* cursor = NULL;

    /* create the dup database file */
    r = db_env_create(&env, 0);        assert(r == 0);
    env->set_errfile(env, stderr);
    r = env->set_generate_row_callback_for_put(env, put_multiple_generate);
    CKERR(r);
    r = env->open(env, TOKU_TEST_FILENAME, DB_INIT_MPOOL|DB_CREATE|DB_THREAD |DB_INIT_LOCK|DB_INIT_LOG|DB_INIT_TXN|DB_PRIVATE, S_IRWXU+S_IRWXG+S_IRWXO); CKERR(r);
    r = db_create(&db, env, 0); assert(r == 0);
    db->set_errfile(db,stderr); // Turn off those annoying errors
    r = db->open(db, null_txn, fname, "main", DB_BTREE, DB_CREATE, 0666); assert(r == 0);

    r = env->txn_begin(env, NULL, &txn, 0);                                                               
    CKERR(r);

    r = env->create_loader(env, txn, &loader, db, 1, &db, &db_flags, &dbt_flags, loader_flags);
    CKERR(r);

    DBT key, val;
    uint32_t k;
    uint32_t v;
    uint32_t num_elements = 2;
    for (uint32_t i = 0; i < num_elements; i++) {
        k = i;
        v = i;
        r = loader->put(
            loader, 
            dbt_init(&key, &k, sizeof k), 
            dbt_init(&val, &v, sizeof v)
            );
        assert(r == 0); 
    }
    if (abort_loader) {
        loader->abort(loader);
    }
    else {
        loader->close(loader);
    }
    k = num_elements;
    v = num_elements;
    r = db->put(db, txn, dbt_init(&key, &k, sizeof k), dbt_init(&val, &v, sizeof v), 0);

    if (abort_txn) {
        r = txn->abort(txn);
        CKERR(r);
    }
    else {
        r = txn->commit(txn, 0);
        CKERR(r);
    }
    
    r = env->txn_begin(env, NULL, &txn, 0);                                                               
    CKERR(r);
    r = db->cursor(db, txn, &cursor, 0); assert(r == 0);
    DBT k1; memset(&k1, 0, sizeof k1);
    DBT v1; memset(&v1, 0, sizeof v1);
    if (!abort_txn) {
        if (!abort_loader) {
            for (uint32_t i = 0; i < num_elements; i++) {
                r = cursor->c_get(cursor, &k1, &v1, DB_NEXT); assert(r == 0);
                assert(k1.size == sizeof(uint32_t));
                assert(v1.size == sizeof(uint32_t));
                assert(*(uint32_t *)k1.data == i);
                assert(*(uint32_t *)v1.data == i);
            }
        }
        r = cursor->c_get(cursor, &k1, &v1, DB_NEXT); assert(r == 0);
        assert(k1.size == sizeof(uint32_t));
        assert(v1.size == sizeof(uint32_t));
        assert(*(uint32_t *)k1.data == num_elements);
        assert(*(uint32_t *)v1.data == num_elements);
    }
    r = cursor->c_get(cursor, &k1, &v1, DB_NEXT); assert(r == DB_NOTFOUND);

    r = cursor->c_close(cursor); assert(r == 0);
    r = txn->commit(txn, 0);
    CKERR(r);

    r = db->close(db, 0); assert(r == 0);
    r = env->close(env, 0); assert(r == 0);
}

int
test_main(int argc, char *const argv[]) {
    parse_args(argc, argv);
    test_loader_abort(false, false, true);
    test_loader_abort(false, true, true);
    test_loader_abort(true, false, true);
    test_loader_abort(true, true, true);
    test_loader_abort(false, false, false);
    test_loader_abort(false, true, false);
    test_loader_abort(true, false, false);
    test_loader_abort(true, true, false);
    return 0;
}
