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
#include <sys/stat.h>
#include <fcntl.h>

// Return the offset
static int
grep_for_in_logs (const char *str) {
#define lfname "log000000000000.tokulog[0-9]*"
#define COMMAND "grep -F -q"
    char lname[TOKU_PATH_MAX+1];
    toku_path_join(lname, 2, TOKU_TEST_FILENAME, lfname);
    char cmd[strlen(str) + sizeof(COMMAND " \"\" ") + TOKU_PATH_MAX];
    int bytes = snprintf(cmd, sizeof(cmd), COMMAND " \"%s\" %s", str, lname);
    assert(bytes>=0);
    assert((size_t)bytes<sizeof(cmd));
    int r = system(cmd);
    assert(r!=-1);
    if (r>0) r = -1;
    return r;
}

int
test_main (int UU(argc), char UU(*const argv[])) {
    int r;
    DB_ENV *env;
    DB *db;
    DB_TXN *tid;

    toku_os_recursive_delete(TOKU_TEST_FILENAME);
    r=toku_os_mkdir(TOKU_TEST_FILENAME, S_IRWXU+S_IRWXG+S_IRWXO);       assert(r==0);
    r=db_env_create(&env, 0); assert(r==0);
    r=env->open(env, TOKU_TEST_FILENAME, DB_INIT_LOCK|DB_INIT_LOG|DB_INIT_MPOOL|DB_INIT_TXN|DB_CREATE|DB_PRIVATE, S_IRWXU+S_IRWXG+S_IRWXO); CKERR(r);
    r=db_create(&db, env, 0); CKERR(r);
    r=env->txn_begin(env, 0, &tid, 0); assert(r==0);
    r=db->open(db, tid, "foo.db", 0, DB_BTREE, DB_CREATE, S_IRWXU+S_IRWXG+S_IRWXO); CKERR(r);
    r=tid->commit(tid, 0);    assert(r==0);

    {
	DBT key,data;
	char hello[]="hello";
	char there[]="there";
	r=env->txn_begin(env, 0, &tid, 0); CKERR(r);
	r=db->put(db, tid,
		  dbt_init(&key, hello, sizeof(hello)),
		  dbt_init(&data, there, sizeof(there)),
		  0);
	r=grep_for_in_logs(hello);
	assert(r==-1);
	r=env->log_flush(env, 0); CKERR(r);
	r=grep_for_in_logs(hello);
	assert(r>=0);
	r=tid->commit(tid, 0);    CKERR(r);
    }
    r=db->close(db, 0); assert(r==0);
    r=env->close(env, 0); assert(r==0);
    return 0;
}
