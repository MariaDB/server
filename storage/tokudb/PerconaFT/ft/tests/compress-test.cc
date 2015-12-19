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

// Test zlib, lzma, quicklz, and snappy.
// Compare to compress-test which tests the toku compression (which is a composite of quicklz and zlib).

#include <sys/time.h>
#include "test.h"
#include "serialize/compress.h"

static float tdiff (struct timeval *start, struct timeval *end) {
    return (end->tv_sec-start->tv_sec) + 1e-6*(end->tv_usec - start->tv_usec);
}

static uLongf test_compress_buf_method (unsigned char *buf, int i, enum toku_compression_method m) {
    int bound = toku_compress_bound(m, i);
    unsigned char *MALLOC_N(bound, cb);
    uLongf actual_clen = bound;
    toku_compress(m, cb, &actual_clen, buf, i);
    unsigned char *MALLOC_N(i, ubuf);
    toku_decompress(ubuf, i, cb, actual_clen);
    assert(0==memcmp(ubuf, buf, i));
    toku_free(ubuf);
    toku_free(cb);
    return actual_clen;
}

static void test_compress_i (int i, enum toku_compression_method m, uLongf *compress_size, uLongf *uncompress_size) {
    unsigned char *MALLOC_N(i, b);
    for (int j=0; j<i; j++) b[j] = random()%256;
    *compress_size += test_compress_buf_method (b, i, m);
    *uncompress_size += i;

    for (int j=0; j<i; j++) b[j] = 0;
    *compress_size += test_compress_buf_method (b, i, m);
    *uncompress_size += i;

    for (int j=0; j<i; j++) b[j] = 0xFF;
    *compress_size += test_compress_buf_method(b, i, m);
    *uncompress_size += i;

    toku_free(b);
}

static void test_compress (enum toku_compression_method m, uLongf *compress_size, uLongf *uncompress_size) {
    // unlike quicklz, we can handle length 0.
    for (int i=0; i<100; i++) {
        test_compress_i(i, m, compress_size, uncompress_size);
    }
    test_compress_i(1024, m, compress_size, uncompress_size);
    test_compress_i(1024*1024*4, m, compress_size, uncompress_size);
    test_compress_i(1024*1024*4 - 123, m, compress_size, uncompress_size); // just some random lengths
}

static void test_compress_methods () {
    struct timeval start, end;
    uLongf compress_size = 0;
    uLongf uncompress_size = 0;

    gettimeofday(&start, NULL);
    test_compress(TOKU_ZLIB_METHOD, &compress_size, &uncompress_size);
    gettimeofday(&end, NULL);
    printf("TOKU_ZLIB_METHOD Time=%.6fs , Ratio=%.2f[%d/%d]\n",
            tdiff(&start, &end),
            (float)compress_size / (float)uncompress_size, (int)compress_size, (int)uncompress_size);

    compress_size = 0;
    uncompress_size = 0;
    gettimeofday(&start, NULL);
    test_compress(TOKU_ZLIB_WITHOUT_CHECKSUM_METHOD, &compress_size, &uncompress_size);
    gettimeofday(&end, NULL);
    printf("TOKU_ZLIB_WITHOUT_CHECKSUM_METHOD Time=%.6fs, Ratio=%.2f[%d/%d]\n",
            tdiff(&start, &end),
            (float)compress_size / (float)uncompress_size, (int)compress_size, (int)uncompress_size);

    compress_size = 0;
    uncompress_size = 0;
    gettimeofday(&start, NULL);
    test_compress(TOKU_QUICKLZ_METHOD, &compress_size, &uncompress_size);
    gettimeofday(&end, NULL);
    printf("TOKU_QUICKLZ_METHOD Time=%.6fs, Ratio=%.2f[%d/%d]\n",
            tdiff(&start, &end),
            (float)compress_size / (float)uncompress_size, (int)compress_size, (int)uncompress_size);

    compress_size = 0;
    uncompress_size = 0;
    gettimeofday(&start, NULL);
    test_compress(TOKU_LZMA_METHOD, &compress_size, &uncompress_size);
    gettimeofday(&end, NULL);
    printf("TOKU_LZMA_METHOD Time=%.6fs, Ratio=%.2f[%d/%d]\n",
            tdiff(&start, &end),
            (float)compress_size / (float)uncompress_size, (int)compress_size, (int)uncompress_size);

    compress_size = 0;
    uncompress_size = 0;
    gettimeofday(&start, NULL);
    test_compress(TOKU_SNAPPY_METHOD, &compress_size, &uncompress_size);
    gettimeofday(&end, NULL);
    printf("TOKU_SNAPPY_METHOD Time=%.6fs, Ratio=%.2f[%d/%d]\n",
            tdiff(&start, &end),
            (float)compress_size / (float)uncompress_size, (int)compress_size, (int)uncompress_size);
}

int test_main (int argc, const char *argv[]) {
    default_parse_args(argc, argv);
    
    test_compress_methods();

    return 0;
}
