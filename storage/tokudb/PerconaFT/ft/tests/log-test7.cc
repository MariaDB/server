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

// create and close, making sure that everything is deallocated properly.

#define LSIZE 100
#define NUM_LOGGERS 10
TOKULOGGER logger[NUM_LOGGERS];

static void setup_logger(int which) {
    char logname[10];
    snprintf(logname, sizeof(logname), "log%d", which);
    char dnamewhich[TOKU_PATH_MAX+1];
    int r;
    toku_path_join(dnamewhich, 2, TOKU_TEST_FILENAME, logname);
    r = toku_os_mkdir(dnamewhich, S_IRWXU);
    if (r!=0) {
        int er = get_error_errno();
        printf("file %s error (%d) %s\n", dnamewhich, er, strerror(er));
        assert(r==0);
    }
    r = toku_logger_create(&logger[which]);
    assert(r == 0);
    r = toku_logger_set_lg_max(logger[which], LSIZE);
    {
	uint32_t n;
	r = toku_logger_get_lg_max(logger[which], &n);
	assert(n==LSIZE);
    }
    r = toku_logger_open(dnamewhich, logger[which]);
    assert(r == 0);
}

static void play_with_logger(int which) {
    {
	ml_lock(&logger[which]->input_lock);
	int lsize=LSIZE-12-2;
	toku_logger_make_space_in_inbuf(logger[which], lsize);
	snprintf(logger[which]->inbuf.buf+logger[which]->inbuf.n_in_buf, lsize, "a%*d", lsize-1, 0);
	logger[which]->inbuf.n_in_buf += lsize;
	logger[which]->lsn.lsn++;
	logger[which]->inbuf.max_lsn_in_buf = logger[which]->lsn;
	ml_unlock(&logger[which]->input_lock);
    }

    {
	ml_lock(&logger[which]->input_lock);
	toku_logger_make_space_in_inbuf(logger[which], 2);
	memcpy(logger[which]->inbuf.buf+logger[which]->inbuf.n_in_buf, "b1", 2);
	logger[which]->inbuf.n_in_buf += 2;
	logger[which]->lsn.lsn++;
	logger[which]->inbuf.max_lsn_in_buf = logger[which]->lsn;
	ml_unlock(&logger[which]->input_lock);
    }
}

static void tear_down_logger(int which) {
    int r;
    r = toku_logger_close(&logger[which]);
    assert(r == 0);
}

int
test_main (int argc __attribute__((__unused__)),
	  const char *argv[] __attribute__((__unused__))) {
    int i;
    int loop;
    const int numloops = 100;
    for (loop = 0; loop < numloops; loop++) {
        toku_os_recursive_delete(TOKU_TEST_FILENAME);
        int r = toku_os_mkdir(TOKU_TEST_FILENAME, S_IRWXU);
        assert_zero(r);
        for (i = 0; i < NUM_LOGGERS; i++) setup_logger(i);
        for (i = 0; i < NUM_LOGGERS; i++) play_with_logger(i);
        for (i = 0; i < NUM_LOGGERS; i++) tear_down_logger(i);
    }
    toku_os_recursive_delete(TOKU_TEST_FILENAME);

    return 0;
}
