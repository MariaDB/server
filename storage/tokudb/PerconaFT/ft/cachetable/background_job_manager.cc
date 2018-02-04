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

#include <portability/toku_config.h>
#include <memory.h>
#include <toku_pthread.h>

#include "cachetable/background_job_manager.h"

toku_instr_key *bjm_jobs_lock_mutex_key;
toku_instr_key *bjm_jobs_wait_key;

struct background_job_manager_struct {
    bool accepting_jobs;
    uint32_t num_jobs;
    toku_cond_t jobs_wait;
    toku_mutex_t jobs_lock;
};

void bjm_init(BACKGROUND_JOB_MANAGER *pbjm) {
    BACKGROUND_JOB_MANAGER XCALLOC(bjm);
    toku_mutex_init(*bjm_jobs_lock_mutex_key, &bjm->jobs_lock, nullptr);
    toku_cond_init(*bjm_jobs_wait_key, &bjm->jobs_wait, nullptr);
    bjm->accepting_jobs = true;
    bjm->num_jobs = 0;
    *pbjm = bjm;
}

void bjm_destroy(BACKGROUND_JOB_MANAGER bjm) {
    assert(bjm->num_jobs == 0);
    toku_cond_destroy(&bjm->jobs_wait);
    toku_mutex_destroy(&bjm->jobs_lock);
    toku_free(bjm);
}

void bjm_reset(BACKGROUND_JOB_MANAGER bjm) {
    toku_mutex_lock(&bjm->jobs_lock);
    assert(bjm->num_jobs == 0);
    bjm->accepting_jobs = true;
    toku_mutex_unlock(&bjm->jobs_lock);
}

int bjm_add_background_job(BACKGROUND_JOB_MANAGER bjm) {
    int ret_val;
    toku_mutex_lock(&bjm->jobs_lock);
    if (bjm->accepting_jobs) {
        bjm->num_jobs++;
        ret_val = 0;
    }
    else {
        ret_val = -1;
    }
    toku_mutex_unlock(&bjm->jobs_lock);
    return ret_val;
}
void bjm_remove_background_job(BACKGROUND_JOB_MANAGER bjm){
    toku_mutex_lock(&bjm->jobs_lock);
    assert(bjm->num_jobs > 0);
    bjm->num_jobs--;
    if (bjm->num_jobs == 0 && !bjm->accepting_jobs) {
        toku_cond_broadcast(&bjm->jobs_wait);
    }
    toku_mutex_unlock(&bjm->jobs_lock);
}

void bjm_wait_for_jobs_to_finish(BACKGROUND_JOB_MANAGER bjm) {
    toku_mutex_lock(&bjm->jobs_lock);
    bjm->accepting_jobs = false;
    while (bjm->num_jobs > 0) {
        toku_cond_wait(&bjm->jobs_wait, &bjm->jobs_lock);
    }
    toku_mutex_unlock(&bjm->jobs_lock);
}

