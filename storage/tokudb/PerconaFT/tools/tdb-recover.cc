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

/* Recover an env.  The logs are in argv[1].  The new database is created in the cwd. */

// Test:
//    cd ../src/tests/tmpdir
//    ../../../ft/recover ../dir.test_log2.c.tdb

#include "ft/ft-ops.h"
#include "ft/logger/recover.h"

static int recovery_main(int argc, const char *const argv[]);

int main(int argc, const char *const argv[]) {
    int r = toku_ft_layer_init();
    assert(r == 0);
    r = recovery_main(argc, argv);
    toku_ft_layer_destroy();
    return r;
}

int recovery_main (int argc, const char *const argv[]) {
    const char *data_dir, *log_dir;
    if (argc==3) {
	data_dir = argv[1];
	log_dir  = argv[2];
    } else if (argc==2) {
	data_dir = log_dir = argv[1];
    } else {
	printf("Usage: %s <datadir> [ <logdir> ]\n", argv[0]);
	return(1);
    }

    int r = tokuft_recover(nullptr,
			   nullptr,
			   nullptr,
			   nullptr,
			   data_dir, log_dir, nullptr, nullptr, nullptr, nullptr, 0);
    if (r!=0) {
	fprintf(stderr, "Recovery failed\n");
	return(1);
    }
    return 0;
}
