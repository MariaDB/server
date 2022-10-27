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

#pragma once

//
// The background job manager keeps track of the existence of 
// background jobs running. We use the background job manager
// to allow threads to perform background jobs on various pieces 
// of the system (e.g. cachefiles and cloned pairs being written out 
// for checkpoint)
//

typedef struct background_job_manager_struct *BACKGROUND_JOB_MANAGER;


void bjm_init(BACKGROUND_JOB_MANAGER* bjm);
void bjm_destroy(BACKGROUND_JOB_MANAGER bjm);

//
// Re-allows a background job manager to accept background jobs
//
void bjm_reset(BACKGROUND_JOB_MANAGER bjm);

//
// add a background job. If return value is 0, then the addition of the job
// was successful and the user may perform the background job. If return
// value is non-zero, then adding of the background job failed and the user
// may not perform the background job.
//
int bjm_add_background_job(BACKGROUND_JOB_MANAGER bjm);

//
// remove a background job
//
void bjm_remove_background_job(BACKGROUND_JOB_MANAGER bjm);

//
// This function waits for all current background jobs to be removed. If the user
// calls bjm_add_background_job while this function is running, or after this function
// has completed, bjm_add_background_job returns an error. 
//
void bjm_wait_for_jobs_to_finish(BACKGROUND_JOB_MANAGER bjm);
