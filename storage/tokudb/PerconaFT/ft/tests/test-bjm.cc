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

#include "cachetable/background_job_manager.h"

#include "test.h"

BACKGROUND_JOB_MANAGER bjm; 

static void *finish_bjm(void *arg) {
    bjm_wait_for_jobs_to_finish(bjm);
    return arg;
}


static void bjm_test(void) {
    int r = 0;
    bjm = NULL;
    bjm_init(&bjm);
    // test simple add/remove of background job works
    r = bjm_add_background_job(bjm);
    assert_zero(r);
    bjm_remove_background_job(bjm);
    bjm_wait_for_jobs_to_finish(bjm);
    // assert that you cannot add a background job
    // without resetting bjm after waiting 
    // for finish
    r = bjm_add_background_job(bjm);
    assert(r != 0);
    // test that after a reset, we can resume adding background jobs
    bjm_reset(bjm);
    r = bjm_add_background_job(bjm);
    assert_zero(r);
    bjm_remove_background_job(bjm);    
    bjm_wait_for_jobs_to_finish(bjm);

    bjm_reset(bjm);
    r = bjm_add_background_job(bjm);
    assert_zero(r);        
    toku_pthread_t tid;    
    r = toku_pthread_create(&tid, NULL, finish_bjm, NULL); 
    assert_zero(r);
    usleep(2*1024*1024);
    // should return non-zero because tid is waiting 
    // for background jobs to finish
    r = bjm_add_background_job(bjm);
    assert(r != 0);
    bjm_remove_background_job(bjm);
    void *ret;
    r = toku_pthread_join(tid, &ret); 
    assert_zero(r);
    
    bjm_destroy(bjm);
}

int
test_main (int argc , const char *argv[]) {
    default_parse_args(argc, argv);
    
    bjm_test();
    if (verbose) printf("test ok\n");
    return 0;
}


