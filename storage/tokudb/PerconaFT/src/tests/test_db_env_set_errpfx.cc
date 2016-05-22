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

#include <db.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/types.h>

// TOKU_TEST_FILENAME is defined in the Makefile

int
test_main(int argc, char *const argv[]) {
    parse_args(argc, argv);
    DB_ENV *dbenv;
    int r;

    toku_os_recursive_delete(TOKU_TEST_FILENAME);
    toku_os_mkdir(TOKU_TEST_FILENAME, S_IRWXU+S_IRWXG+S_IRWXO);

    r = db_env_create(&dbenv, 0);
    assert(r == 0);

    dbenv->set_errpfx(dbenv, "houdy partners");

    r = dbenv->open(dbenv, TOKU_TEST_FILENAME, DB_CREATE|DB_PRIVATE|DB_INIT_MPOOL, 0);
    assert(r == 0);

    dbenv->set_errpfx(dbenv, "houdy partners");

    r = dbenv->close(dbenv, 0);
    assert(r == 0);

    return 0;
}
