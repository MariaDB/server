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

#include <toku_pthread.h>
#include <toku_time.h>

// Specification:
// A minicron is a miniature cron job for executing a job periodically inside a pthread.
// To create a minicron,
//   1) allocate a "struct minicron" somewhere.
//      Rationale:  This struct can be stored inside another struct (such as the cachetable), avoiding a malloc/free pair.
//   2) call toku_minicron_setup, specifying a period (in milliseconds), a function, and some arguments.
//      If the period is positive then the function is called periodically (with the period specified)
//      Note: The period is measured from when the previous call to f finishes to when the new call starts.
//            Thus, if the period is 5 minutes, and it takes 8 minutes to run f, then the actual periodicity is 13 minutes.
//      Rationale:  If f always takes longer than f to run, then it will get "behind".  This module makes getting behind explicit.
//   3) When finished, call toku_minicron_shutdown.
//   4) If you want to change the period, then call toku_minicron_change_period.    The time since f finished is applied to the new period
//      and the call is rescheduled.  (If the time since f finished is more than the new period, then f is called immediately).

struct minicron {
    toku_pthread_t thread;
    toku_timespec_t time_of_last_call_to_f;
    toku_mutex_t mutex;
    toku_cond_t  condvar;
    int (*f)(void*);
    void *arg;
    uint32_t period_in_ms;
    bool      do_shutdown;
};

int toku_minicron_setup (struct minicron *s, uint32_t period_in_ms, int(*f)(void *), void *arg);
void toku_minicron_change_period(struct minicron *p, uint32_t new_period);
uint32_t toku_minicron_get_period_in_seconds_unlocked(struct minicron *p);
uint32_t toku_minicron_get_period_in_ms_unlocked(struct minicron *p);
int toku_minicron_shutdown(struct minicron *p);
bool toku_minicron_has_been_shutdown(struct minicron *p);
