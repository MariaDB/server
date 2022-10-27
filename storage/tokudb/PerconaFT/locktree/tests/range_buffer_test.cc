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

#include <string.h>

#include <portability/memory.h>

#include <locktree/range_buffer.h>

namespace toku {

const size_t num_points = 60;

static const DBT *get_dbt_by_iteration(size_t i) {
    if (i == 0) {
        return toku_dbt_negative_infinity();
    } else if (i < (num_points - 1)) {
        return get_dbt(i); 
    } else {
        return toku_dbt_positive_infinity();
    }
}

static void test_points(void) {
    range_buffer buffer;
    buffer.create();

    for (size_t i = 0; i < num_points; i++) {
        const DBT *point = get_dbt_by_iteration(i);
        buffer.append(point, point);
    }

    size_t i = 0;
    range_buffer::iterator iter(&buffer);
    range_buffer::iterator::record rec;
    while (iter.current(&rec)) {
        const DBT *expected_point = get_dbt_by_iteration(i);
        invariant(compare_dbts(nullptr, expected_point, rec.get_left_key()) == 0);
        invariant(compare_dbts(nullptr, expected_point, rec.get_right_key()) == 0);
        iter.next();
        i++;
    }
    invariant(i == num_points);

    buffer.destroy();
}

static void test_ranges(void) {
    range_buffer buffer;
    buffer.create();

    // we are going to store adjacent points as ranges,
    // so make sure there are an even number of points.
    invariant(num_points % 2 == 0);

    for (size_t i = 0; i < num_points; i += 2) {
        const DBT *left = get_dbt_by_iteration(i);
        const DBT *right = get_dbt_by_iteration(i + 1);
        buffer.append(left, right);
    }

    size_t i = 0;
    range_buffer::iterator iter(&buffer);
    range_buffer::iterator::record rec;
    while (iter.current(&rec)) {
        const DBT *expected_left = get_dbt_by_iteration(i);
        const DBT *expected_right = get_dbt_by_iteration(i + 1);
        invariant(compare_dbts(nullptr, expected_left, rec.get_left_key()) == 0);
        invariant(compare_dbts(nullptr, expected_right, rec.get_right_key()) == 0);
        iter.next();
        i += 2;
    }
    invariant(i == num_points);

    buffer.destroy();

}

static void test_mixed(void) {
    range_buffer buffer;
    buffer.create();
    buffer.destroy();

    // we are going to store adjacent points as ranges,
    // followed by a single point, so make sure the
    // number of points is a multiple of 3.
    invariant(num_points % 3 == 0);

    for (size_t i = 0; i < num_points; i += 3) {
        const DBT *left = get_dbt_by_iteration(i);
        const DBT *right = get_dbt_by_iteration(i + 1);
        const DBT *point = get_dbt_by_iteration(i + 2);
        buffer.append(left, right);
        buffer.append(point, point);
    }

    size_t i = 0;
    range_buffer::iterator iter(&buffer);
    range_buffer::iterator::record rec;
    while (iter.current(&rec)) {
        const DBT *expected_left = get_dbt_by_iteration(i);
        const DBT *expected_right = get_dbt_by_iteration(i + 1);
        invariant(compare_dbts(nullptr, expected_left, rec.get_left_key()) == 0);
        invariant(compare_dbts(nullptr, expected_right, rec.get_right_key()) == 0);
        iter.next();

        const DBT *expected_point = get_dbt_by_iteration(i + 2);
        bool had_point = iter.current(&rec);
        invariant(had_point);
        invariant(compare_dbts(nullptr, expected_point, rec.get_left_key()) == 0);
        invariant(compare_dbts(nullptr, expected_point, rec.get_right_key()) == 0);
        iter.next();
        i += 3;
    }
    invariant(i == num_points);

    buffer.destroy();
}

static void test_small_and_large_points(void) {
    range_buffer buffer;
    buffer.create();
    buffer.destroy();

    // Test a bug where a small append would cause
    // the range buffer to not grow properly for
    // a subsequent large append.
    const size_t small_size = 32;
    const size_t large_size = 16 * 1024;
    char *small_buf = (char *) toku_xmalloc(small_size);
    char *large_buf = (char *) toku_xmalloc(large_size);
    DBT small_dbt, large_dbt;
    memset(&small_dbt, 0, sizeof(DBT));
    memset(&large_dbt, 0, sizeof(DBT));
    small_dbt.data = small_buf;
    small_dbt.size = small_size;
    large_dbt.data = large_buf;
    large_dbt.size = large_size;

    // Append a small dbt, the buf should be able to fit it.
    buffer.append(&small_dbt, &small_dbt);
    invariant(buffer.total_memory_size() >= small_dbt.size);
    // Append a large dbt, the buf should be able to fit it.
    buffer.append(&large_dbt, &large_dbt);
    invariant(buffer.total_memory_size() >= (small_dbt.size + large_dbt.size));

    toku_free(small_buf);
    toku_free(large_buf);
    buffer.destroy();
}

} /* namespace toku */

int main(void) {
    toku::test_points();
    toku::test_ranges();
    toku::test_mixed();
    toku::test_small_and_large_points();
    return 0;
}
