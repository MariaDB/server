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

/* Simple test of logging.  Can I start PerconaFT with logging enabled? */

#include <errno.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <db.h>
#include <memory.h>
#include <stdio.h>


// TOKU_TEST_FILENAME is defined in the Makefile

static void
test_db_open_aborts (void) {
    DB_ENV *env;
    DB *db;

    int r;
    toku_os_recursive_delete(TOKU_TEST_FILENAME);
    r=toku_os_mkdir(TOKU_TEST_FILENAME, S_IRWXU+S_IRWXG+S_IRWXO);       assert(r==0);
    r=db_env_create(&env, 0); assert(r==0);
    r=env->open(env, TOKU_TEST_FILENAME, DB_INIT_LOCK|DB_INIT_LOG|DB_INIT_MPOOL|DB_INIT_TXN|DB_PRIVATE|DB_CREATE, S_IRWXU+S_IRWXG+S_IRWXO); CKERR(r);
    r=db_create(&db, env, 0); CKERR(r);
    {
	DB_TXN *tid;
	r=env->txn_begin(env, 0, &tid, 0); assert(r==0);
	r=db->open(db, tid, "foo.db", 0, DB_BTREE, DB_CREATE, S_IRWXU|S_IRWXG|S_IRWXO); CKERR(r);
	{
	    DBT key,data;
            dbt_init(&key, "hello", 6);
            dbt_init(&data, "there", 6);
	    r=db->put(db, tid, &key, &data, 0);
	    CKERR(r);
	}
        r=db->close(db, 0);       assert(r==0);
	r=tid->abort(tid);        assert(r==0);
    }
    {
        {
            DBT dname;
            DBT iname;
            dbt_init(&dname, "foo.db", sizeof("foo.db"));
            dbt_init(&iname, NULL, 0);
            iname.flags |= DB_DBT_MALLOC;
            r = env->get_iname(env, &dname, &iname);
            CKERR2(r, DB_NOTFOUND);
        }
        toku_struct_stat statbuf;
        char filename[TOKU_PATH_MAX + 1];
        r = toku_stat(toku_path_join(filename, 2, TOKU_TEST_FILENAME, "foo.db"),
                      &statbuf,
                      toku_uninstrumented);
        assert(r != 0);
        assert(errno == ENOENT);
    }

    r=env->close(env, 0);     assert(r==0);
}

// Do two transactions, one commits, and one aborts.  Do them concurrently.
static void
test_db_put_aborts (void) {
    DB_ENV *env;
    DB *db;

    int r;
    toku_os_recursive_delete(TOKU_TEST_FILENAME);
    r=toku_os_mkdir(TOKU_TEST_FILENAME, S_IRWXU+S_IRWXG+S_IRWXO);       assert(r==0);
    r=db_env_create(&env, 0); assert(r==0);
    r=env->open(env, TOKU_TEST_FILENAME, DB_INIT_LOCK|DB_INIT_LOG|DB_INIT_MPOOL|DB_INIT_TXN|DB_PRIVATE|DB_CREATE, S_IRWXU+S_IRWXG+S_IRWXO); CKERR(r);
    r=db_create(&db, env, 0); CKERR(r);

    {
	DB_TXN *tid;
	r=env->txn_begin(env, 0, &tid, 0); assert(r==0);
	r=db->open(db, tid, "foo.db", 0, DB_BTREE, DB_CREATE, S_IRWXU+S_IRWXG+S_IRWXO); CKERR(r);
	r=tid->commit(tid,0);        assert(r==0);
    }
    {
	DB_TXN *tid;
	DB_TXN *tid2;
	r=env->txn_begin(env, 0, &tid, 0);  assert(r==0);
	r=env->txn_begin(env, 0, &tid2, 0); assert(r==0);
	{
	    DBT key,data;
            dbt_init(&key, "hello", 6);
            dbt_init(&data, "there", 6);
	    r=db->put(db, tid, &key, &data, 0);
	    CKERR(r);
	}
	{
	    DBT key,data;
            dbt_init(&key, "bye", 4);
            dbt_init(&data, "now", 4);
	    r=db->put(db, tid2, &key, &data, 0);
	    CKERR(r);
	}
	//printf("%s:%d aborting\n", __FILE__, __LINE__);
	r=tid->abort(tid);        assert(r==0);
	//printf("%s:%d committing\n", __FILE__, __LINE__);
	r=tid2->commit(tid2,0);   assert(r==0);
    }
    // The database should exist
    {
        char *filename;
        {
            DBT dname;
            DBT iname;
            dbt_init(&dname, "foo.db", sizeof("foo.db"));
            dbt_init(&iname, NULL, 0);
            iname.flags |= DB_DBT_MALLOC;
            r = env->get_iname(env, &dname, &iname);
            CKERR(r);
            CAST_FROM_VOIDP(filename, iname.data);
            assert(filename);
        }
        toku_struct_stat statbuf;
        char fullfile[TOKU_PATH_MAX + 1];
        r = toku_stat(toku_path_join(fullfile, 2, TOKU_TEST_FILENAME, filename),
                      &statbuf,
                      toku_uninstrumented);
        assert(r == 0);
        toku_free(filename);
    }
    // But the item should not be in it.
    if (1)
    {
	DB_TXN *tid;
	r=env->txn_begin(env, 0, &tid, 0); assert(r==0);
	{
	    DBT key,data;
            dbt_init(&key, "hello", 6);
            dbt_init(&data, NULL, 0);
	    r=db->get(db, tid, &key, &data, 0);
	    assert(r!=0);
	    assert(r==DB_NOTFOUND);
	}	    
	{
	    DBT key,data;
            dbt_init(&key, "bye", 4);
            dbt_init(&data, NULL, 0);
	    r=db->get(db, tid, &key, &data, 0);
	    CKERR(r);
	}	    
	r=tid->commit(tid,0);        assert(r==0);
    }

    r=db->close(db, 0);       assert(r==0);
    r=env->close(env, 0);     assert(r==0);
}

int
test_main (int UU(argc), char UU(*const argv[])) {
    test_db_open_aborts();
    test_db_put_aborts();
    return 0;
}
