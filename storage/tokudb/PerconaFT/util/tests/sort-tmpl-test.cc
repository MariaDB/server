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

#include <stdlib.h>

#include <memory.h>
#include <util/sort.h>

const int MAX_NUM = 0x0fffffffL;
int MAGIC_EXTRA = 0xd3adb00f;

static int
int_qsort_cmp(const void *va, const void *vb) {
    const int *CAST_FROM_VOIDP(a, va);
    const int *CAST_FROM_VOIDP(b, vb);
    assert(*a < MAX_NUM);
    assert(*b < MAX_NUM);
    return (*a > *b) - (*a < *b);
}

int int_cmp(const int &e, const int &a, const int &b);
int
int_cmp(const int &e, const int &a, const int &b)
{
    assert(e == MAGIC_EXTRA);
    return int_qsort_cmp(&a, &b);
}

static void
check_int_array(int a[], int nelts)
{
    assert(a[0] < MAX_NUM);
    for (int i = 1; i < nelts; ++i) {
        assert(a[i] < MAX_NUM);
        assert(a[i-1] <= a[i]);
    }
}

static void
zero_array_test(void)
{
    int unused = MAGIC_EXTRA - 1;
    toku::sort<int, const int, int_cmp>::mergesort_r(NULL, 0, unused);
}

static void
dup_array_test(int nelts)
{
    int *XMALLOC_N(nelts, a);
    for (int i = 0; i < nelts; ++i) {
        a[i] = 1;
    }
    toku::sort<int, const int, int_cmp>::mergesort_r(a, nelts, MAGIC_EXTRA);
    check_int_array(a, nelts);
    toku_free(a);
}

static void
already_sorted_test(int nelts)
{
    int *XMALLOC_N(nelts, a);
    for (int i = 0; i < nelts; ++i) {
        a[i] = i;
    }
    toku::sort<int, const int, int_cmp>::mergesort_r(a, nelts, MAGIC_EXTRA);
    check_int_array(a, nelts);
    toku_free(a);
}

static void
random_array_test(int nelts)
{
    int *XMALLOC_N(nelts, a);
    int *XMALLOC_N(nelts, b);
    for (int i = 0; i < nelts; ++i) {
        a[i] = rand() % MAX_NUM;
        b[i] = a[i];
    }
    toku::sort<int, const int, int_cmp>::mergesort_r(a, nelts, MAGIC_EXTRA);
    check_int_array(a, nelts);
    qsort(b, nelts, sizeof b[0], int_qsort_cmp);
    for (int i = 0; i < nelts; ++i) {
        assert(a[i] == b[i]);
    }
    toku_free(a);
    toku_free(b);
}

static int
uint64_qsort_cmp(const void *va, const void *vb) {
    const uint64_t *CAST_FROM_VOIDP(a, va);
    const uint64_t *CAST_FROM_VOIDP(b, vb);
    return (*a > *b) - (*a < *b);
}

int uint64_cmp(const int &e, const uint64_t &a, const uint64_t &b);
int
uint64_cmp(const int &e, const uint64_t &a, const uint64_t &b)
{
    assert(e == MAGIC_EXTRA);
    return uint64_qsort_cmp(&a, &b);
}

static void
random_array_test_64(int nelts)
{
    uint64_t *XMALLOC_N(nelts, a);
    uint64_t *XMALLOC_N(nelts, b);
    for (int i = 0; i < nelts; ++i) {
        a[i] = ((uint64_t)rand() << 32ULL) | rand();
        b[i] = a[i];
    }
    toku::sort<uint64_t, const int, uint64_cmp>::mergesort_r(a, nelts, MAGIC_EXTRA);
    qsort(b, nelts, sizeof b[0], uint64_qsort_cmp);
    for (int i = 0; i < nelts; ++i) {
        assert(a[i] == b[i]);
    }
    toku_free(a);
    toku_free(b);
}

int
test_main(int argc __attribute__((__unused__)), const char *argv[] __attribute__((__unused__)))
{
    zero_array_test();
    random_array_test(10);
    random_array_test(1000);
    random_array_test(10001);
    random_array_test(19999);
    random_array_test(39999);
    random_array_test(10000000);
    random_array_test_64(10000000);
    dup_array_test(10);
    dup_array_test(1000);
    dup_array_test(10001);
    dup_array_test(10000000);
    already_sorted_test(10);
    already_sorted_test(1000);
    already_sorted_test(10001);
    already_sorted_test(10000000);
    return 0;
}
