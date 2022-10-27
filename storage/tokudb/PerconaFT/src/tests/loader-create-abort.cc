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

// Ensure that loader->abort free all of its resources.  The test just creates a loader and then
// aborts it.

#include "test.h"
#include <db.h>

static int loader_flags = 0;
static const char *envdir = TOKU_TEST_FILENAME;

static int put_multiple_generate(DB *UU(dest_db), DB *UU(src_db), DBT_ARRAY *UU(dest_keys), DBT_ARRAY *UU(dest_vals), const DBT *UU(src_key), const DBT *UU(src_val)) {
    return ENOMEM;
}

static void loader_open_abort(void) {
    int r;

    char rmcmd[32 + strlen(envdir)];
    snprintf(rmcmd, sizeof rmcmd, "rm -rf %s", envdir);
    r = system(rmcmd);                                                                             CKERR(r);
    r = toku_os_mkdir(envdir, S_IRWXU+S_IRWXG+S_IRWXO);                                                       CKERR(r);

    DB_ENV *env;
    r = db_env_create(&env, 0);                                                                               CKERR(r);
    r = env->set_generate_row_callback_for_put(env, put_multiple_generate);
    CKERR(r);
    int envflags = DB_INIT_LOCK | DB_INIT_LOG | DB_INIT_MPOOL | DB_INIT_TXN | DB_CREATE | DB_PRIVATE;
    r = env->open(env, envdir, envflags, S_IRWXU+S_IRWXG+S_IRWXO);                                            CKERR(r); 
    env->set_errfile(env, stderr);

    DB_TXN *txn;
    r = env->txn_begin(env, NULL, &txn, 0); CKERR(r);

    DB_LOADER *loader;
    r = env->create_loader(env, txn, &loader, NULL, 0, NULL, NULL, NULL, loader_flags); CKERR(r);
    
    r = loader->abort(loader); CKERR(r);

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
	    fprintf(stderr, "Usage: [-h] [-v] [-q] [-p]\n%s\n", cmd);
	    exit(resultcode);
        } else if (strcmp(argv[0], "-v")==0) {
	    verbose++;
	} else if (strcmp(argv[0],"-q")==0) {
	    verbose--;
	    if (verbose<0) verbose=0;
        } else if (strcmp(argv[0], "-p") == 0) {
            loader_flags |= LOADER_COMPRESS_INTERMEDIATES;
        } else if (strcmp(argv[0], "-z") == 0) {
            loader_flags |= LOADER_DISALLOW_PUTS;
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
    loader_open_abort();
    return 0;
}
