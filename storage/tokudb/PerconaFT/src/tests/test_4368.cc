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


// TOKU_TEST_FILENAME is defined in the Makefile


int
test_main (int UU(argc), char UU(*const argv[])) {
  int r;
  DB_ENV *env;
  DB *db;
  toku_os_recursive_delete(TOKU_TEST_FILENAME);
  r=toku_os_mkdir(TOKU_TEST_FILENAME, S_IRWXU+S_IRWXG+S_IRWXO);       assert(r==0);
  r=db_env_create(&env, 0); assert(r==0);
  r=env->open(env, TOKU_TEST_FILENAME, DB_PRIVATE|DB_CREATE, S_IRWXU+S_IRWXG+S_IRWXO); assert(r==0);
  r=db_create(&db, env, 0); assert(r==0);
  r = db->open(db, NULL, "test.db", 0, DB_BTREE, DB_CREATE, S_IRWXU+S_IRWXG+S_IRWXO); assert(r == 0);

  // call hot_optimize on an empty db. The empty db should have only a root node, which should invoke the bug
  uint64_t loops_run;
  r = db->hot_optimize(db, NULL, NULL, NULL, NULL, &loops_run); assert_zero(r);
    
  r=db->close(db, 0);       assert(r==0);
  r=env->close(env, 0);     assert(r==0);
  return 0;
}
