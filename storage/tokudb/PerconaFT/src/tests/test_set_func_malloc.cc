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

#define DONT_DEPRECATE_MALLOC
#include "test.h"

/* Test to see if setting malloc works. */

#include <memory.h>
#include <db.h>

static int malloc_counter=0;
static int realloc_counter=0;
static int free_counter=0;

static void *
bmalloc (size_t s)
{
    malloc_counter++;
    return malloc(s);
}

static void
bfree (void*p)
{
    free_counter++;
    free(p);
}

static void*
brealloc (void*p, size_t s)
{
    realloc_counter++;
    return realloc(p,s);
}

static void
test1 (void)
{
    DB_ENV *env=0;
    int r;
    r = db_env_create(&env, 0);            assert(r==0);
    r = env->close(env, 0);                assert(r==0);
    assert(malloc_counter==0);
    assert(free_counter==0);
    assert(realloc_counter==0);

    db_env_set_func_malloc(bmalloc);
    r = db_env_create(&env, 0);            assert(r==0);
    r = env->close(env, 0);                assert(r==0);
    assert(malloc_counter>0);
    assert(free_counter==0);
    assert(realloc_counter==0);

    malloc_counter = realloc_counter = free_counter = 0;

    db_env_set_func_free(bfree);
    db_env_set_func_malloc(NULL);
    r = db_env_create(&env, 0);            assert(r==0);
    r = env->close(env, 0);                assert(r==0);
    assert(malloc_counter==0);
    assert(free_counter>=0);
    assert(realloc_counter==0);
    
    db_env_set_func_malloc(bmalloc);
    db_env_set_func_realloc(brealloc);
    db_env_set_func_free(bfree);
    
    // toku_malloc isn't affected by calling the BDB set_fun_malloc calls.
    malloc_counter = realloc_counter = free_counter = 0;

    {
	void *x = toku_malloc(5); assert(x);	assert(malloc_counter==1 && free_counter==0 && realloc_counter==0);
	x = toku_realloc(x, 6);   assert(x);    assert(malloc_counter==1 && free_counter==0 && realloc_counter==1);
	toku_free(x);	                        assert(malloc_counter==1 && free_counter==1 && realloc_counter==1);
    }

    db_env_set_func_malloc(NULL);
    db_env_set_func_realloc(NULL);
    db_env_set_func_free(NULL);
}

int
test_main (int argc __attribute__((__unused__)), char *const argv[] __attribute__((__unused__)))
{
    test1();
    return 0;
}
