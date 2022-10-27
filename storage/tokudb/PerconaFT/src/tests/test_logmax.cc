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
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>

static void
check_logmax (int max) {
    int any_too_big=0;
    DIR *dir = opendir(TOKU_TEST_FILENAME);
    struct dirent *ent;
    while ((ent=readdir(dir))) {
	if ((ent->d_type==DT_REG || ent->d_type==DT_UNKNOWN) && strncmp(ent->d_name, "log", 3)==0) {
        // It is a "log*" file
        char full_fname[TOKU_PATH_MAX + 1];
        toku_struct_stat sbuf;
        int r = toku_stat(
            toku_path_join(full_fname, 2, TOKU_TEST_FILENAME, ent->d_name),
            &sbuf,
            toku_uninstrumented);
        assert(r == 0);
        if (verbose)
            printf("%s is of size %" PRId64 "\n",
                   ent->d_name,
                   (int64_t)sbuf.st_size);
        if (sbuf.st_size > max) any_too_big=1;
	}
    }
    assert(!any_too_big);
    int r=closedir(dir);
    assert(r==0);
}

static void
test_logmax (int logmax) {
    int r;
    DB_ENV *env;
    DB *db;
    DB_TXN *tid;

    toku_os_recursive_delete(TOKU_TEST_FILENAME);
    r=toku_os_mkdir(TOKU_TEST_FILENAME, S_IRWXU+S_IRWXG+S_IRWXO);       assert(r==0);
    r=db_env_create(&env, 0); assert(r==0);
    if (logmax>0) {
	r=env->set_lg_max(env, logmax);
	assert(r==0);
    }
    r=env->open(env, TOKU_TEST_FILENAME, DB_INIT_LOCK|DB_INIT_LOG|DB_INIT_MPOOL|DB_INIT_TXN|DB_CREATE|DB_PRIVATE, S_IRWXU+S_IRWXG+S_IRWXO); CKERR(r);
    {
	uint32_t lmax;
	r=env->get_lg_max(env, &lmax);
	assert(r==0);
	if (logmax>0) {
	    assert(lmax==(uint32_t)logmax);
	} else {
	    assert(lmax>0);
	    
	}
    }
    r=db_create(&db, env, 0); CKERR(r);
    r=env->txn_begin(env, 0, &tid, 0); assert(r==0);
    r=db->open(db, tid, "foo.db", 0, DB_BTREE, DB_CREATE, S_IRWXU+S_IRWXG+S_IRWXO); CKERR(r);
    r=tid->commit(tid, 0);    assert(r==0);

    int i;
    int sum = 0;
    int effective_max;
    if (logmax>0) effective_max = logmax;
    else {
	effective_max = 100<<20;
    }

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
    r=db->close(db, 0); assert(r==0);
    r=env->close(env, 0); assert(r==0);
    check_logmax(effective_max);
}

int
test_main (int argc, char *const argv[]) {
    parse_args(argc, argv);
    test_logmax(1<<20);
    test_logmax(-1);
    return 0;
}
