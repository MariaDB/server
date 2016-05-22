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

static void
test_create (void) {
    message_buffer msg_buffer;
    msg_buffer.create();
    msg_buffer.destroy();
}

static char *buildkey(size_t len) {
    char *XMALLOC_N(len, k);
    memset(k, 0, len);
    return k;
}

static char *buildval(size_t len) {
    char *XMALLOC_N(len, v);
    memset(v, ~len, len);
    return v;
}

static void
test_enqueue(int n) {
    MSN startmsn = ZERO_MSN;

    message_buffer msg_buffer;
    msg_buffer.create();

    for (int i=0; i<n; i++) {
        int thekeylen = i + 1;
        int thevallen = i + 2;
        char *thekey = buildkey(thekeylen);
        char *theval = buildval(thevallen);
        XIDS xids;
        if (i == 0) {
            xids = toku_xids_get_root_xids();
        } else {
            int r = toku_xids_create_child(toku_xids_get_root_xids(), &xids, (TXNID)i);
            assert_zero(r);
        }
        MSN msn = next_dummymsn();
        if (startmsn.msn == ZERO_MSN.msn)
            startmsn = msn;
        enum ft_msg_type type = (enum ft_msg_type) i;
        DBT k, v;
        ft_msg msg(toku_fill_dbt(&k, thekey, thekeylen), toku_fill_dbt(&v, theval, thevallen), type, msn, xids);
        msg_buffer.enqueue(msg, true, nullptr);
        toku_xids_destroy(&xids);
        toku_free(thekey);
        toku_free(theval);
    }

    struct checkit_fn {
        MSN startmsn;
        int verbose;
        int i;
        checkit_fn(MSN smsn, bool v)
            : startmsn(smsn), verbose(v), i(0) {
        }
        int operator()(const ft_msg &msg, bool UU(is_fresh)) {
            int thekeylen = i + 1;
            int thevallen = i + 2;
            char *thekey = buildkey(thekeylen);
            char *theval = buildval(thevallen);

            MSN msn = msg.msn();
            enum ft_msg_type type = msg.type();
            if (verbose) printf("checkit %d %d %" PRIu64 "\n", i, type, msn.msn);
            assert(msn.msn == startmsn.msn + i);
            assert((int) msg.kdbt()->size == thekeylen); assert(memcmp(msg.kdbt()->data, thekey, msg.kdbt()->size) == 0);
            assert((int) msg.vdbt()->size == thevallen); assert(memcmp(msg.vdbt()->data, theval, msg.vdbt()->size) == 0);
            assert(i % 256 == (int)type);
            assert((TXNID)i == toku_xids_get_innermost_xid(msg.xids()));
            i += 1;
            toku_free(thekey);
            toku_free(theval);
            return 0;
        }
    } checkit(startmsn, verbose);
    msg_buffer.iterate(checkit);
    assert(checkit.i == n);

    msg_buffer.destroy();
}

int
test_main(int argc, const char *argv[]) {
    default_parse_args(argc, argv);
    initialize_dummymsn();
    test_create();
    test_enqueue(4);
    test_enqueue(512);
    
    return 0;
}
