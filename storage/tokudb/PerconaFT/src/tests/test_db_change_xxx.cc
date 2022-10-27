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

/* Can I close a db without opening it? */

#include <stdlib.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <db.h>


int
test_main (int UU(argc), char UU(*const argv[])) {
    int r;
    DB_ENV *env;
    DB *db;
    toku_os_recursive_delete(TOKU_TEST_FILENAME);
    r=toku_os_mkdir(TOKU_TEST_FILENAME, S_IRWXU+S_IRWXG+S_IRWXO);       assert(r==0);
    r=db_env_create(&env, 0); assert(r==0);
    r=env->open(env, TOKU_TEST_FILENAME, DB_PRIVATE|DB_CREATE, S_IRWXU+S_IRWXG+S_IRWXO); assert(r==0);

    uint32_t ret_val = 0;
    r = db_create(&db, env, 0);
    CKERR(r);
    r = db->set_pagesize(db, 112024);
    CKERR(r);
    r = db->change_pagesize(db, 202433);
    CKERR2(r, EINVAL);
    r = db->get_pagesize(db, &ret_val);
    CKERR(r);
    assert(ret_val == 112024);
    r = db->set_readpagesize(db, 33024);
    CKERR(r);
    r = db->change_readpagesize(db, 202433);
    CKERR2(r, EINVAL);
    r = db->get_readpagesize(db, &ret_val);
    CKERR(r);
    assert(ret_val == 33024);

    enum toku_compression_method method = TOKU_ZLIB_METHOD;
    enum toku_compression_method ret_method = TOKU_NO_COMPRESSION;
    r = db->set_compression_method(db, method);
    CKERR(r);
    r = db->change_compression_method(db, method);
    CKERR2(r, EINVAL);
    r = db->get_compression_method(db, &ret_method);
    CKERR(r);
    assert(ret_method == TOKU_ZLIB_METHOD);

    // now do the open
    const char * const fname = "test.change_xxx";
    r = db->open(db, NULL, fname, "main", DB_BTREE, DB_CREATE, 0666);
    CKERR(r);
    
    r = db->get_pagesize(db, &ret_val);
    CKERR(r);
    assert(ret_val == 112024);
    r = db->get_readpagesize(db, &ret_val);
    CKERR(r);
    assert(ret_val == 33024);
    ret_method = TOKU_NO_COMPRESSION;
    r = db->get_compression_method(db, &ret_method);
    CKERR(r);
    assert(ret_method == TOKU_ZLIB_METHOD);

    r = db->set_pagesize(db, 2024);
    CKERR2(r, EINVAL);
    r = db->set_readpagesize(db, 1111);
    CKERR2(r, EINVAL);
    r = db->set_compression_method(db, TOKU_NO_COMPRESSION);
    CKERR2(r, EINVAL);

    r = db->change_pagesize(db, 100000);
    CKERR(r);
    r = db->change_readpagesize(db, 10000);
    CKERR(r);
    r = db->change_compression_method(db, TOKU_LZMA_METHOD);
    CKERR(r);
    
    r = db->get_pagesize(db, &ret_val);
    CKERR(r);
    assert(ret_val == 100000);
    r = db->get_readpagesize(db, &ret_val);
    CKERR(r);
    assert(ret_val == 10000);
    ret_method = TOKU_NO_COMPRESSION;
    r = db->get_compression_method(db, &ret_method);
    CKERR(r);
    assert(ret_method == TOKU_LZMA_METHOD);

    r = db->close(db, 0);
    
    r = db_create(&db, env, 0);
    CKERR(r);
    r = db->open(db, NULL, fname, "main", DB_BTREE, DB_AUTO_COMMIT, 0666);
    CKERR(r);

    r = db->get_pagesize(db, &ret_val);
    CKERR(r);
    assert(ret_val == 100000);
    r = db->get_readpagesize(db, &ret_val);
    CKERR(r);
    assert(ret_val == 10000);
    ret_method = TOKU_NO_COMPRESSION;
    r = db->get_compression_method(db, &ret_method);
    CKERR(r);
    assert(ret_method == TOKU_LZMA_METHOD);

    r = db->close(db, 0);

    r=env->close(env, 0);     assert(r==0);
    return 0;
}
