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


// Try to open an environment where the directory does not exist 
// Try when the dir exists but is not an initialized env
// Try when the dir exists and we do DB_CREATE: it should work.
// And after that the open should work without a DB_CREATE
//   However, in BDB, after doing an DB_ENV->open and then a close, no state has changed
//   One must actually create a DB I think...


#include <db.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/types.h>

#include <unistd.h>

// TOKU_TEST_FILENAME is defined in the Makefile

int
test_main(int argc, char *const argv[]) {
    parse_args(argc, argv);
    DB_ENV *dbenv;
    int r;
    int do_private;

    for (do_private=0; do_private<2; do_private++) {
	if (do_private==0) continue; // See #208.
	int private_flags = do_private ? (DB_CREATE|DB_PRIVATE) : 0;
	
	toku_os_recursive_delete(TOKU_TEST_FILENAME);
	r = db_env_create(&dbenv, 0);
	CKERR(r);
	r = dbenv->open(dbenv, TOKU_TEST_FILENAME, private_flags|DB_INIT_MPOOL, 0);
	assert(r==ENOENT);
	dbenv->close(dbenv,0); // free memory
	
	toku_os_recursive_delete(TOKU_TEST_FILENAME);
	toku_os_mkdir(TOKU_TEST_FILENAME, S_IRWXU+S_IRWXG+S_IRWXO);
	r = db_env_create(&dbenv, 0);
	CKERR(r);
	r = dbenv->open(dbenv, TOKU_TEST_FILENAME, private_flags|DB_INIT_MPOOL, 0);
	// PerconaFT has no trouble opening an environment if the directory exists.
	CKERR(r);
	assert(r==0);
	dbenv->close(dbenv,0); // free memory
    }

    return 0;
}

