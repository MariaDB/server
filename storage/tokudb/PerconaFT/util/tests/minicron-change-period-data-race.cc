/* -*- mode: C++; c-basic-offset: 4; indent-tabs-mode: nil -*- */
// vim: ft=cpp:expandtab:ts=8:sw=4:softtabstop=4:
#ident "$Id$"
/*======
This file is part of PerconaFT.


Copyright (c) 2018, Percona and/or its affiliates. All rights reserved.

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

#ident "Copyright (c) 2018, Percona and/or its affiliates. All rights reserved."

#include <toku_portability.h>
#include "test.h"
#include "util/minicron.h"
#include <unistd.h>
#include <string.h>
#include <stdlib.h>

// The thread sanitizer detected a data race in the minicron in a test unrelated to the minicron.
// This test reproduces the data race in a much smaller test which merely runs minicron tasks
// while changing the minicron period in an unrelated thread.

static int do_nothing(void *UU(v)) {
    return 0;
}

int test_main (int argc, const char *argv[]) {
    default_parse_args(argc,argv);

    minicron m = {};
    int r = toku_minicron_setup(&m, 1, do_nothing, nullptr);
    assert(r == 0);
    for (int i=0; i<1000; i++) 
        toku_minicron_change_period(&m, 1);
    r = toku_minicron_shutdown(&m);
    assert(r == 0);

    return 0;
}
