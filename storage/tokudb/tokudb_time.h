/* -*- mode: C++; c-basic-offset: 4; indent-tabs-mode: nil -*- */
// vim: ft=cpp:expandtab:ts=8:sw=4:softtabstop=4:
/* -*- mode: C; c-basic-offset: 4 -*- */
#ident "$Id$"
/*======
This file is part of TokuDB


Copyright (c) 2006, 2015, Percona and/or its affiliates. All rights reserved.

    TokuDBis is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License, version 2,
    as published by the Free Software Foundation.

    TokuDB is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with TokuDB.  If not, see <http://www.gnu.org/licenses/>.

======= */

#ident "Copyright (c) 2006, 2015, Percona and/or its affiliates. All rights reserved."

#ifndef _TOKUDB_TIME_H
#define _TOKUDB_TIME_H

#include "hatoku_defines.h"

namespace tokudb {
namespace time {

static const ulonglong MILLISECONDS = 1000;
static const ulonglong MICROSECONDS = 1000000;
static const ulonglong NANOSECONDS = 1000000000;

// gets curent time of day in microseconds
ulonglong microsec(void);

// gets a timespec in the future based on the current time and an offset forward
timespec offset_timespec(ulonglong offset);

// sleep microseconds
void sleep_microsec(ulong tm);



inline ulonglong microsec(void) {
    timeval t;
    gettimeofday(&t, NULL);
    return t.tv_sec * (1UL * 1000 * 1000) + t.tv_usec;
}
inline timespec offset_timespec(ulonglong offset) {
    timespec ret;
    ulonglong tm = offset + microsec();
    ret.tv_sec = tm / MICROSECONDS;
    ret.tv_nsec = (tm % MICROSECONDS) * 1000;
    return ret;
}
inline void sleep_microsec(ulong tm) {
    timeval	t;
    t.tv_sec = tm / MICROSECONDS;
    t.tv_usec = tm % MICROSECONDS;

    select(0, NULL, NULL, NULL, &t);
}

} // namespace time
} // namespace tokudb

#endif // _TOKUDB_TIME_H
