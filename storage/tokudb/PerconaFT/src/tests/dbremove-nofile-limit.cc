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

// This test verifies that the env->dbremove function returns an error rather than
// crash when the NOFILE resource limit is exceeded.

#include "test.h"
#include <db.h>
#include <sys/resource.h>

static const char *envdir = TOKU_TEST_FILENAME;

static void test_dbremove() {
    int r;

    char rmcmd[32 + strlen(envdir)];
    snprintf(rmcmd, sizeof rmcmd, "rm -rf %s", envdir);
    r = system(rmcmd);                                                                             CKERR(r);
    r = toku_os_mkdir(envdir, S_IRWXU+S_IRWXG+S_IRWXO);                                                       CKERR(r);

    DB_ENV *env;
    r = db_env_create(&env, 0);                                                                               CKERR(r);
    int envflags = DB_INIT_LOCK | DB_INIT_LOG | DB_INIT_MPOOL | DB_INIT_TXN | DB_CREATE | DB_PRIVATE;
    r = env->open(env, envdir, envflags, S_IRWXU+S_IRWXG+S_IRWXO);                                            CKERR(r);
    env->set_errfile(env, stderr);

    DB *db;
    r = db_create(&db, env, 0); CKERR(r);
    char fname[32];
    sprintf(fname, "db%d", 0);
    r = db->open(db, nullptr, fname, nullptr, DB_BTREE, DB_CREATE, 0666); CKERR(r);

    r = db->close(db, 0); CKERR(r);

    DB_TXN *txn;
    r = env->txn_begin(env, nullptr, &txn, 0); CKERR(r);

    struct rlimit current_limit;
    r = getrlimit(RLIMIT_NOFILE, &current_limit);
    assert(r == 0);
    
    struct rlimit new_limit = current_limit;
    new_limit.rlim_cur = 0;
    r = setrlimit(RLIMIT_NOFILE, &new_limit);
    assert(r == 0);

    r = env->dbremove(env, txn, fname, nullptr, 0);
    CKERR2(r, EMFILE);

    r = setrlimit(RLIMIT_NOFILE, &current_limit);
    assert(r == 0);

    r = env->dbremove(env, txn, fname, nullptr, 0);
    CKERR(r);
    
    r = txn->commit(txn, 0); CKERR(r);

    r = env->close(env, 0); CKERR(r);
}

static void do_args(int argc, char * const argv[]) {
    int resultcode;
    char *cmd = argv[0];
    argc--; argv++;
    while (argc>0) {
        if (strcmp(argv[0], "-h")==0) {
	    resultcode=0;
	do_usage:
	    fprintf(stderr, "Usage: %s -h -v -q\n", cmd);
	    exit(resultcode);
	} else if (strcmp(argv[0], "-v")==0) {
	    verbose++;
	} else if (strcmp(argv[0],"-q")==0) {
	    verbose--;
	    if (verbose<0) verbose=0;
	} else {
	    fprintf(stderr, "Unknown arg: %s\n", argv[0]);
	    resultcode=1;
	    goto do_usage;
	}
	argc--;
	argv++;
    }
}

int test_main(int argc, char * const *argv) {
    do_args(argc, argv);
    test_dbremove();
    return 0;
}
