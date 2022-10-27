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

// Verify that env->create_loader works correctly (does not crash, does not leak memory, returns the right error code)
// when the NPROC limit is exceeded.

#include "test.h"
#include <db.h>
#include <sys/resource.h>

static int loader_flags = 0;
static const char *envdir = TOKU_TEST_FILENAME;

static void run_test(int ndb) {
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

    DB *dbs[ndb];
    uint32_t db_flags[ndb];
    uint32_t dbt_flags[ndb];
    for (int i = 0; i < ndb; i++) {
        db_flags[i] = DB_NOOVERWRITE;
        dbt_flags[i] = 0;
        r = db_create(&dbs[i], env, 0); CKERR(r);
        char name[32];
        sprintf(name, "db%d", i);
        r = dbs[i]->open(dbs[i], NULL, name, NULL, DB_BTREE, DB_CREATE, 0666); CKERR(r);
    }

    DB_TXN *txn;
    r = env->txn_begin(env, NULL, &txn, 0); CKERR(r);

    struct rlimit current_nproc_limit;
    r = getrlimit(RLIMIT_NPROC, &current_nproc_limit);
    assert(r == 0);
    
    struct rlimit new_nproc_limit = current_nproc_limit;
    new_nproc_limit.rlim_cur = 0;
    r = setrlimit(RLIMIT_NPROC, &new_nproc_limit);
    assert(r == 0);

    DB_LOADER *loader;
    int loader_r = env->create_loader(env, txn, &loader, ndb > 0 ? dbs[0] : NULL, ndb, dbs, db_flags, dbt_flags, loader_flags);

    r = setrlimit(RLIMIT_NPROC, &current_nproc_limit);
    assert(r == 0);

    if (loader_flags & LOADER_DISALLOW_PUTS)  {
        CKERR(loader_r);
        loader_r = loader->close(loader);
        CKERR(loader_r);
    } else {
        CKERR2(loader_r, EAGAIN);
    }

    r = txn->abort(txn); CKERR(r);

    for (int i = 0; i < ndb; i++) {
        r = dbs[i]->close(dbs[i], 0); CKERR(r);
    }

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
	    fprintf(stderr, "Usage: %s -h -v -q -p\n", cmd);
	    exit(resultcode);
	} else if (strcmp(argv[0], "-v")==0) {
	    verbose++;
	} else if (strcmp(argv[0],"-q")==0) {
	    verbose--;
	    if (verbose<0) verbose=0;
        } else if (strcmp(argv[0], "-p") == 0) {
            loader_flags |= LOADER_DISALLOW_PUTS;
        } else if (strcmp(argv[0], "-z") == 0) {
            loader_flags |= LOADER_COMPRESS_INTERMEDIATES;
        } else if (strcmp(argv[0], "-e") == 0) {
            argc--; argv++;
            if (argc > 0)
                envdir = argv[0];
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
    run_test(1);
    return 0;
}
