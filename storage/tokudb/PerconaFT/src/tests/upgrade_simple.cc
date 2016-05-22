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


/* Purpose of this test is to verify simplest part of upgrade logic.
 * Start by creating two very simple 4.x environments,
 * one in each of two states:
 *  - after a clean shutdown
 *  - without a clean shutdown
 *
 * The two different environments will be used to exercise upgrade logic
 * for 5.x.
 *
 */


#include "test.h"
#include <db.h>

static DB_ENV *env;

#define FLAGS_NOLOG DB_INIT_LOCK|DB_INIT_MPOOL|DB_CREATE|DB_PRIVATE
#define FLAGS_LOG   FLAGS_NOLOG|DB_INIT_TXN|DB_INIT_LOG

static int mode = S_IRWXU+S_IRWXG+S_IRWXO;

static void test_shutdown(void);

#define OLDDATADIR "../../../../tokudb.data/"

static char *env_dir = TOKU_TEST_FILENAME; // the default env_dir.

static char * dir_v41_clean = OLDDATADIR "env_simple.4.1.1.cleanshutdown";
static char * dir_v42_clean = OLDDATADIR "env_simple.4.2.0.cleanshutdown";
static char * dir_v42_dirty = OLDDATADIR "env_simple.4.2.0.dirtyshutdown";
static char * dir_v41_dirty_multilogfile = OLDDATADIR "env_preload.4.1.1.multilog.dirtyshutdown";
static char * dir_v42_dirty_multilogfile = OLDDATADIR "env_preload.4.2.0.multilog.dirtyshutdown";


static void
setup (uint32_t flags, bool clean, bool too_old, char * src_db_dir) {
    int r;
    int len = 256;
    char syscmd[len];

    if (env)
        test_shutdown();

    r = snprintf(syscmd, len, "rm -rf %s", env_dir);
    assert(r<len);
    r = system(syscmd);                                                                                  
    CKERR(r);
    
    r = snprintf(syscmd, len, "cp -r %s %s", src_db_dir, env_dir);
    assert(r<len);
    r = system(syscmd);                                                                                 
    CKERR(r);

    r=db_env_create(&env, 0); 
    CKERR(r);
    env->set_errfile(env, stderr);
    r=env->open(env, TOKU_TEST_FILENAME, flags, mode); 
    if (clean)
	CKERR(r);
    else {
	if (too_old)
	    CKERR2(r, TOKUDB_DICTIONARY_TOO_OLD);
	else
	    CKERR2(r, TOKUDB_UPGRADE_FAILURE);
    }
}



static void
test_shutdown(void) {
    int r;
    r=env->close(env, 0); CKERR(r);
    env = NULL;
}


static void
test_env_startup(void) {
    uint32_t flags;
    
    flags = FLAGS_LOG;

    setup(flags, true, false, dir_v42_clean);
    print_engine_status(env);
    test_shutdown();

    setup(flags, false, true, dir_v41_clean);
    print_engine_status(env);
    test_shutdown();

    setup(flags, false, false, dir_v42_dirty);
    if (verbose) {
	printf("\n\nEngine status after aborted env->open() will have some garbage values:\n");
    }
    print_engine_status(env);
    test_shutdown();

    setup(flags, false, true, dir_v41_dirty_multilogfile);
    if (verbose) {
	printf("\n\nEngine status after aborted env->open() will have some garbage values:\n");
    }
    print_engine_status(env);
    test_shutdown();

    setup(flags, false, false, dir_v42_dirty_multilogfile);
    if (verbose) {
	printf("\n\nEngine status after aborted env->open() will have some garbage values:\n");
    }
    print_engine_status(env);
    test_shutdown();
}


int
test_main (int argc, char * const argv[]) {
    parse_args(argc, argv);
    test_env_startup();
    return 0;
}
