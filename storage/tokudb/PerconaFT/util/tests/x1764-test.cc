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
#include <util/x1764.h>

static void
test0 (void) {
    uint32_t c = toku_x1764_memory("", 0);
    assert(c==~(0U));
    struct x1764 cs;
    toku_x1764_init(&cs);
    toku_x1764_add(&cs, "", 0);
    c = toku_x1764_finish(&cs);
    assert(c==~(0U));
}

static void
test1 (void) {
    uint64_t v=0x123456789abcdef0ULL;
    uint32_t c;
    int i;
    for (i=0; i<=8; i++) {
	uint64_t expect64 = (i==8) ? v : v&((1LL<<(8*i))-1);
	uint32_t expect = expect64 ^ (expect64>>32);
	c = toku_x1764_memory(&v, i);
	//printf("i=%d c=%08x expect=%08x\n", i, c, expect);
	assert(c==~expect);
    }
}

// Compute checksums incrementally, using various strides
static void
test2 (void) {
    enum { N=200 };
    char v[N];
    int i;
    for (i=0; i<N; i++) v[i]=(char)random();
    for (i=0; i<N; i++) {
	int j;
	for (j=i; j<=N; j++) {
	    // checksum from i (inclusive to j (exclusive)
	    uint32_t c = toku_x1764_memory(&v[i], j-i);
	    // Now compute the checksum incrementally with various strides.
	    int stride;
	    for (stride=1; stride<=j-i; stride++) {
		int k;
		struct x1764 s;
		toku_x1764_init(&s);
		for (k=i; k+stride<=j; k+=stride) {
		    toku_x1764_add(&s, &v[k], stride);
		}
		toku_x1764_add(&s, &v[k], j-k);
		uint32_t c2 = toku_x1764_finish(&s);
		assert(c2==c);
	    }
	    // Now use some random strides.
	    {
		int k=i;
		struct x1764 s;
		toku_x1764_init(&s);
		while (1) {
		    stride=random()%16;
		    if (k+stride>j) break;
		    toku_x1764_add(&s, &v[k], stride);
		    k+=stride;
		}
		toku_x1764_add(&s, &v[k], j-k);
		uint32_t c2 = toku_x1764_finish(&s);
		assert(c2==c);
	    }
	}
    }
}

static void
test3 (void)
// Compare the simple version to the highly optimized verison.
{
    const int datalen = 1000;
    char data[datalen];
    for (int i=0; i<datalen; i++) data[i]=random();
    for (int off=0; off<32; off++) {
	if (verbose) {printf("."); fflush(stdout);}
	for (int len=0; len+off<datalen; len++) {
	    uint32_t reference_sum = toku_x1764_memory_simple(data+off, len);
	    uint32_t fast_sum      = toku_x1764_memory       (data+off, len);
	    assert(reference_sum==fast_sum);
	}
    }
}

int
test_main (int argc __attribute__((__unused__)), const char *argv[] __attribute__((__unused__))) {
    if (verbose) printf("0\n");
    test0();
    if (verbose) printf("1\n");
    test1();
    if (verbose) printf("2\n");
    test2();
    if (verbose) printf("3\n");
    test3();
    return 0;
}
