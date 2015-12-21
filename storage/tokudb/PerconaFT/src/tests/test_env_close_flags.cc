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
#include <errno.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/types.h>


// TOKU_TEST_FILENAME is defined in the Makefile

int
test_main (int argc __attribute__((__unused__)), char *const argv[]  __attribute__((__unused__))) {
    DB_ENV *env;
    int r;
    toku_os_recursive_delete(TOKU_TEST_FILENAME);
    r=toku_os_mkdir(TOKU_TEST_FILENAME, S_IRWXU+S_IRWXG+S_IRWXO);        assert(r==0);
    r=db_env_create(&env, 0);  assert(r==0);
    env->set_errfile(env,0); // Turn off those annoying errors
    r=env->close   (env, 0);   assert(r==0);
    toku_os_recursive_delete(TOKU_TEST_FILENAME);
    r=toku_os_mkdir(TOKU_TEST_FILENAME, S_IRWXU+S_IRWXG+S_IRWXO);        assert(r==0);
    r=db_env_create(&env, 0);  assert(r==0);
    env->set_errfile(env,0); // Turn off those annoying errors
    r=env->close   (env, 1);  
    assert(r==EINVAL);
    toku_os_recursive_delete(TOKU_TEST_FILENAME);
    r=toku_os_mkdir(TOKU_TEST_FILENAME, S_IRWXU+S_IRWXG+S_IRWXO);        assert(r==0);

    r=db_env_create(&env, 0);  assert(r==0);
    env->set_errfile(env,0); // Turn off those annoying errors
    r=env->open(env, TOKU_TEST_FILENAME, DB_INIT_LOCK|DB_INIT_LOG|DB_INIT_MPOOL|DB_INIT_TXN|DB_PRIVATE|DB_CREATE, S_IRWXU+S_IRWXG+S_IRWXO); CKERR(r);
    r=env->close   (env, 0);  assert(r==0);
    
    toku_os_recursive_delete(TOKU_TEST_FILENAME);
    r=toku_os_mkdir(TOKU_TEST_FILENAME, S_IRWXU+S_IRWXG+S_IRWXO);        assert(r==0);

    r=db_env_create(&env, 0);  assert(r==0);
    env->set_errfile(env,0); // Turn off those annoying errors
    r=env->open(env, TOKU_TEST_FILENAME, DB_INIT_LOCK|DB_INIT_LOG|DB_INIT_MPOOL|DB_INIT_TXN|DB_PRIVATE|DB_CREATE, S_IRWXU+S_IRWXG+S_IRWXO); CKERR(r);
    r=env->close   (env, 1);
    assert(r==EINVAL);
    return 0;
}
