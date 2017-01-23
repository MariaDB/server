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

#include <unistd.h>
#include <stdlib.h>
#include <sys/time.h>
#include "test.h"

#ifndef MIN
#define MIN(x, y) (((x) < (y)) ? (x) : (y))
#endif
const double USECS_PER_SEC = 1000000.0;

static void le_add_to_bn(bn_data *bn,
                         uint32_t idx,
                         char *key,
                         int keylen,
                         char *val,
                         int vallen) {
    LEAFENTRY r = NULL;
    uint32_t size_needed = LE_CLEAN_MEMSIZE(vallen);
    void *maybe_free = nullptr;
    bn->get_space_for_insert(idx, key, keylen, size_needed, &r, &maybe_free);
    if (maybe_free) {
        toku_free(maybe_free);
    }
    resource_assert(r);
    r->type = LE_CLEAN;
    r->u.clean.vallen = vallen;
    memcpy(r->u.clean.val, val, vallen);
}

static int long_key_cmp(DB *UU(e), const DBT *a, const DBT *b) {
    const long *CAST_FROM_VOIDP(x, a->data);
    const long *CAST_FROM_VOIDP(y, b->data);
    return (*x > *y) - (*x < *y);
}

static void test_serialize_leaf(int valsize,
                                int nelts,
                                double entropy,
                                int ser_runs,
                                int deser_runs) {
    //    struct ft_handle source_ft;
    struct ftnode *sn, *dn;

    int fd = open(TOKU_TEST_FILENAME,
                  O_RDWR | O_CREAT | O_BINARY,
                  S_IRWXU | S_IRWXG | S_IRWXO);
    invariant(fd >= 0);

    int r;

    XCALLOC(sn);

    sn->max_msn_applied_to_node_on_disk.msn = 0;
    sn->flags = 0x11223344;
    sn->blocknum.b = 20;
    sn->layout_version = FT_LAYOUT_VERSION;
    sn->layout_version_original = FT_LAYOUT_VERSION;
    sn->height = 0;
    sn->n_children = 8;
    sn->dirty = 1;
    sn->oldest_referenced_xid_known = TXNID_NONE;
    MALLOC_N(sn->n_children, sn->bp);
    sn->pivotkeys.create_empty();
    for (int i = 0; i < sn->n_children; ++i) {
        BP_STATE(sn, i) = PT_AVAIL;
        set_BLB(sn, i, toku_create_empty_bn());
    }
    int nperbn = nelts / sn->n_children;
    for (int ck = 0; ck < sn->n_children; ++ck) {
        long k;
        for (long i = 0; i < nperbn; ++i) {
            k = ck * nperbn + i;
            char buf[valsize];
            int c;
            for (c = 0; c < valsize * entropy;) {
                int *p = (int *)&buf[c];
                *p = rand();
                c += sizeof(*p);
            }
            memset(&buf[c], 0, valsize - c);
            le_add_to_bn(
                BLB_DATA(sn, ck), i, (char *)&k, sizeof k, buf, sizeof buf);
        }
        if (ck < 7) {
            DBT pivotkey;
            sn->pivotkeys.insert_at(toku_fill_dbt(&pivotkey, &k, sizeof(k)),
                                    ck);
        }
    }

    FT_HANDLE XMALLOC(ft);
    FT XCALLOC(ft_h);
    toku_ft_init(ft_h,
                 make_blocknum(0),
                 ZERO_LSN,
                 TXNID_NONE,
                 4 * 1024 * 1024,
                 128 * 1024,
                 TOKU_DEFAULT_COMPRESSION_METHOD,
                 16);
    ft_h->cmp.create(long_key_cmp, nullptr);
    ft->ft = ft_h;

    ft_h->blocktable.create();
    {
        int r_truncate = ftruncate(fd, 0);
        CKERR(r_truncate);
    }
    // Want to use block #20
    BLOCKNUM b = make_blocknum(0);
    while (b.b < 20) {
        ft_h->blocktable.allocate_blocknum(&b, ft_h);
    }
    invariant(b.b == 20);

    {
        DISKOFF offset;
        DISKOFF size;
        ft_h->blocktable.realloc_on_disk(b, 100, &offset, ft_h, fd, false);
        invariant(offset ==
               (DISKOFF)BlockAllocator::BLOCK_ALLOCATOR_TOTAL_HEADER_RESERVE);

        ft_h->blocktable.translate_blocknum_to_offset_size(b, &offset, &size);
        invariant(offset ==
               (DISKOFF)BlockAllocator::BLOCK_ALLOCATOR_TOTAL_HEADER_RESERVE);
        invariant(size == 100);
    }

    struct timeval total_start;
    struct timeval total_end;
    total_start.tv_sec = total_start.tv_usec = 0;
    total_end.tv_sec = total_end.tv_usec = 0;
    struct timeval t[2];
    FTNODE_DISK_DATA ndd = NULL;
    for (int i = 0; i < ser_runs; i++) {
        gettimeofday(&t[0], NULL);
        ndd = NULL;
        sn->dirty = 1;
        r = toku_serialize_ftnode_to(
            fd, make_blocknum(20), sn, &ndd, true, ft->ft, false);
        invariant(r == 0);
        gettimeofday(&t[1], NULL);
        total_start.tv_sec += t[0].tv_sec;
        total_start.tv_usec += t[0].tv_usec;
        total_end.tv_sec += t[1].tv_sec;
        total_end.tv_usec += t[1].tv_usec;
        toku_free(ndd);
    }
    double dt;
    dt = (total_end.tv_sec - total_start.tv_sec) +
         ((total_end.tv_usec - total_start.tv_usec) / USECS_PER_SEC);
    dt *= 1000;
    dt /= ser_runs;
    printf(
        "serialize leaf(ms):   %0.05lf (average of %d runs)\n", dt, ser_runs);

    // reset
    total_start.tv_sec = total_start.tv_usec = 0;
    total_end.tv_sec = total_end.tv_usec = 0;

    ftnode_fetch_extra bfe;
    for (int i = 0; i < deser_runs; i++) {
        bfe.create_for_full_read(ft_h);
        gettimeofday(&t[0], NULL);
        FTNODE_DISK_DATA ndd2 = NULL;
        r = toku_deserialize_ftnode_from(
            fd, make_blocknum(20), 0 /*pass zero for hash*/, &dn, &ndd2, &bfe);
        invariant(r == 0);
        gettimeofday(&t[1], NULL);

        total_start.tv_sec += t[0].tv_sec;
        total_start.tv_usec += t[0].tv_usec;
        total_end.tv_sec += t[1].tv_sec;
        total_end.tv_usec += t[1].tv_usec;

        toku_ftnode_free(&dn);
        toku_free(ndd2);
    }
    dt = (total_end.tv_sec - total_start.tv_sec) +
         ((total_end.tv_usec - total_start.tv_usec) / USECS_PER_SEC);
    dt *= 1000;
    dt /= deser_runs;
    printf(
        "deserialize leaf(ms): %0.05lf (average of %d runs)\n", dt, deser_runs);
    printf(
        "io time(ms) %lf decompress time(ms) %lf deserialize time(ms) %lf "
        "(average of %d runs)\n",
        tokutime_to_seconds(bfe.io_time) * 1000,
        tokutime_to_seconds(bfe.decompress_time) * 1000,
        tokutime_to_seconds(bfe.deserialize_time) * 1000,
        deser_runs);

    toku_ftnode_free(&sn);

    ft_h->blocktable.block_free(
        BlockAllocator::BLOCK_ALLOCATOR_TOTAL_HEADER_RESERVE, 100);
    ft_h->blocktable.destroy();
    ft_h->cmp.destroy();
    toku_free(ft_h->h);
    toku_free(ft_h);
    toku_free(ft);

    r = close(fd);
    invariant(r != -1);
}

static void test_serialize_nonleaf(int valsize,
                                   int nelts,
                                   double entropy,
                                   int ser_runs,
                                   int deser_runs) {
    //    struct ft_handle source_ft;
    struct ftnode sn, *dn;

    int fd = open(TOKU_TEST_FILENAME,
                  O_RDWR | O_CREAT | O_BINARY,
                  S_IRWXU | S_IRWXG | S_IRWXO);
    invariant(fd >= 0);

    int r;

    //    source_ft.fd=fd;
    sn.max_msn_applied_to_node_on_disk.msn = 0;
    sn.flags = 0x11223344;
    sn.blocknum.b = 20;
    sn.layout_version = FT_LAYOUT_VERSION;
    sn.layout_version_original = FT_LAYOUT_VERSION;
    sn.height = 1;
    sn.n_children = 8;
    sn.dirty = 1;
    sn.oldest_referenced_xid_known = TXNID_NONE;
    MALLOC_N(sn.n_children, sn.bp);
    sn.pivotkeys.create_empty();
    for (int i = 0; i < sn.n_children; ++i) {
        BP_BLOCKNUM(&sn, i).b = 30 + (i * 5);
        BP_STATE(&sn, i) = PT_AVAIL;
        set_BNC(&sn, i, toku_create_empty_nl());
    }
    // Create XIDS
    XIDS xids_0 = toku_xids_get_root_xids();
    XIDS xids_123;
    r = toku_xids_create_child(xids_0, &xids_123, (TXNID)123);
    CKERR(r);
    toku::comparator cmp;
    cmp.create(long_key_cmp, nullptr);
    int nperchild = nelts / 8;
    for (int ck = 0; ck < sn.n_children; ++ck) {
        long k;
        NONLEAF_CHILDINFO bnc = BNC(&sn, ck);
        for (long i = 0; i < nperchild; ++i) {
            k = ck * nperchild + i;
            char buf[valsize];
            int c;
            for (c = 0; c < valsize * entropy;) {
                int *p = (int *)&buf[c];
                *p = rand();
                c += sizeof(*p);
            }
            memset(&buf[c], 0, valsize - c);

            toku_bnc_insert_msg(bnc,
                                &k,
                                sizeof k,
                                buf,
                                valsize,
                                FT_NONE,
                                next_dummymsn(),
                                xids_123,
                                true,
                                cmp);
        }
        if (ck < 7) {
            DBT pivotkey;
            sn.pivotkeys.insert_at(toku_fill_dbt(&pivotkey, &k, sizeof(k)), ck);
        }
    }

    // Cleanup:
    toku_xids_destroy(&xids_0);
    toku_xids_destroy(&xids_123);
    cmp.destroy();

    FT_HANDLE XMALLOC(ft);
    FT XCALLOC(ft_h);
    toku_ft_init(ft_h,
                 make_blocknum(0),
                 ZERO_LSN,
                 TXNID_NONE,
                 4 * 1024 * 1024,
                 128 * 1024,
                 TOKU_DEFAULT_COMPRESSION_METHOD,
                 16);
    ft_h->cmp.create(long_key_cmp, nullptr);
    ft->ft = ft_h;

    ft_h->blocktable.create();
    {
        int r_truncate = ftruncate(fd, 0);
        CKERR(r_truncate);
    }
    // Want to use block #20
    BLOCKNUM b = make_blocknum(0);
    while (b.b < 20) {
        ft_h->blocktable.allocate_blocknum(&b, ft_h);
    }
    invariant(b.b == 20);

    {
        DISKOFF offset;
        DISKOFF size;
        ft_h->blocktable.realloc_on_disk(b, 100, &offset, ft_h, fd, false);
        invariant(offset ==
               (DISKOFF)BlockAllocator::BLOCK_ALLOCATOR_TOTAL_HEADER_RESERVE);

        ft_h->blocktable.translate_blocknum_to_offset_size(b, &offset, &size);
        invariant(offset ==
               (DISKOFF)BlockAllocator::BLOCK_ALLOCATOR_TOTAL_HEADER_RESERVE);
        invariant(size == 100);
    }

    struct timeval t[2];
    gettimeofday(&t[0], NULL);
    FTNODE_DISK_DATA ndd = NULL;
    r = toku_serialize_ftnode_to(
        fd, make_blocknum(20), &sn, &ndd, true, ft->ft, false);
    invariant(r == 0);
    gettimeofday(&t[1], NULL);
    double dt;
    dt = (t[1].tv_sec - t[0].tv_sec) +
         ((t[1].tv_usec - t[0].tv_usec) / USECS_PER_SEC);
    dt *= 1000;
    printf(
        "serialize nonleaf(ms):   %0.05lf (IGNORED RUNS=%d)\n", dt, ser_runs);

    ftnode_fetch_extra bfe;
    bfe.create_for_full_read(ft_h);
    gettimeofday(&t[0], NULL);
    FTNODE_DISK_DATA ndd2 = NULL;
    r = toku_deserialize_ftnode_from(
        fd, make_blocknum(20), 0 /*pass zero for hash*/, &dn, &ndd2, &bfe);
    invariant(r == 0);
    gettimeofday(&t[1], NULL);
    dt = (t[1].tv_sec - t[0].tv_sec) +
         ((t[1].tv_usec - t[0].tv_usec) / USECS_PER_SEC);
    dt *= 1000;
    printf(
        "deserialize nonleaf(ms): %0.05lf (IGNORED RUNS=%d)\n", dt, deser_runs);
    printf(
        "io time(ms) %lf decompress time(ms) %lf deserialize time(ms) %lf "
        "(IGNORED RUNS=%d)\n",
        tokutime_to_seconds(bfe.io_time) * 1000,
        tokutime_to_seconds(bfe.decompress_time) * 1000,
        tokutime_to_seconds(bfe.deserialize_time) * 1000,
        deser_runs);

    toku_ftnode_free(&dn);
    toku_destroy_ftnode_internals(&sn);

    ft_h->blocktable.block_free(
        BlockAllocator::BLOCK_ALLOCATOR_TOTAL_HEADER_RESERVE, 100);
    ft_h->blocktable.destroy();
    toku_free(ft_h->h);
    ft_h->cmp.destroy();
    toku_free(ft_h);
    toku_free(ft);
    toku_free(ndd);
    toku_free(ndd2);

    r = close(fd);
    invariant(r != -1);
}

int test_main(int argc __attribute__((__unused__)),
              const char *argv[] __attribute__((__unused__))) {
    const int DEFAULT_RUNS = 5;
    long valsize, nelts, ser_runs = DEFAULT_RUNS, deser_runs = DEFAULT_RUNS;
    double entropy = 0.3;

    if (argc != 3 && argc != 5) {
        fprintf(stderr,
                "Usage: %s <valsize> <nelts> [<serialize_runs> "
                "<deserialize_runs>]\n",
                argv[0]);
        fprintf(stderr, "Default (and min) runs is %d\n", DEFAULT_RUNS);
        return 2;
    }
    valsize = strtol(argv[1], NULL, 0);
    nelts = strtol(argv[2], NULL, 0);
    if (argc == 5) {
        ser_runs = strtol(argv[3], NULL, 0);
        deser_runs = strtol(argv[4], NULL, 0);
    }

    if (ser_runs <= 0) {
        ser_runs = DEFAULT_RUNS;
    }
    if (deser_runs <= 0) {
        deser_runs = DEFAULT_RUNS;
    }

    initialize_dummymsn();
    test_serialize_leaf(valsize, nelts, entropy, ser_runs, deser_runs);
    test_serialize_nonleaf(valsize, nelts, entropy, ser_runs, deser_runs);

    return 0;
}
