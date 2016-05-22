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

#include <limits.h>

#include "ft/comparator.h"
#include "util/dbt.h"

namespace toku {

    __attribute__((__unused__))
    static DBT min_dbt(void) {
        static int64_t min = INT_MIN;
        DBT dbt;
        toku_fill_dbt(&dbt, &min, sizeof(int64_t));
        dbt.flags = DB_DBT_USERMEM;
        return dbt;
    }

    __attribute__((__unused__))
    static DBT max_dbt(void) {
        static int64_t max = INT_MAX;
        DBT dbt;
        toku_fill_dbt(&dbt, &max, sizeof(int64_t));
        dbt.flags = DB_DBT_USERMEM;
        return dbt;
    }

    __attribute__((__unused__))
    static const DBT *get_dbt(int64_t key) {
        static const int NUM_DBTS = 1000;
        static bool initialized;
        static int64_t static_ints[NUM_DBTS];
        static DBT static_dbts[NUM_DBTS];
        invariant(key < NUM_DBTS);
        if (!initialized) {
            for (int i = 0; i < NUM_DBTS; i++) {
                static_ints[i] = i;
                toku_fill_dbt(&static_dbts[i],
                        &static_ints[i],
                        sizeof(int64_t));
                static_dbts[i].flags = DB_DBT_USERMEM;
            }
            initialized = true;
        }

        invariant(key < NUM_DBTS);
        return &static_dbts[key];
    }

    __attribute__((__unused__))
    static int compare_dbts(DB *db, const DBT *key1, const DBT *key2) {
        (void) db;

        // this emulates what a "infinity-aware" comparator object does
        if (toku_dbt_is_infinite(key1) || toku_dbt_is_infinite(key2)) {
            return toku_dbt_infinite_compare(key1, key2);
        } else {
            invariant(key1->size == sizeof(int64_t));
            invariant(key2->size == sizeof(int64_t));
            int64_t a = *(int64_t*) key1->data;
            int64_t b = *(int64_t*) key2->data;
            if (a < b) {
                return -1;
            } else if (a == b) {
                return 0;
            } else {
                return 1;
            }
        }
    }

    __attribute__((__unused__)) comparator dbt_comparator;

    __attribute__((__constructor__))
    static void construct_dbt_comparator(void) {
        dbt_comparator.create(compare_dbts, nullptr); 
    }

    __attribute__((__destructor__))
    static void destruct_dbt_comparator(void) {
        dbt_comparator.destroy();
    }

} /* namespace toku */
