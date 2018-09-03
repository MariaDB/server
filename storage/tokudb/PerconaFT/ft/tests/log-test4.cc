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

int
test_main (int argc __attribute__((__unused__)),
	  const char *argv[] __attribute__((__unused__))) {
    int r;
    char logname[TOKU_PATH_MAX+1];
    toku_os_recursive_delete(TOKU_TEST_FILENAME);
    r = toku_os_mkdir(TOKU_TEST_FILENAME, S_IRWXU);                               assert(r==0);
    TOKULOGGER logger;
    r = toku_logger_create(&logger);                                 assert(r == 0);
    r = toku_logger_open(TOKU_TEST_FILENAME, logger);                             assert(r == 0);

    {
	ml_lock(&logger->input_lock);
	toku_logger_make_space_in_inbuf(logger, 5);
	memcpy(logger->inbuf.buf+logger->inbuf.n_in_buf, "a1234", 5);
	logger->inbuf.n_in_buf+=5;
	logger->lsn.lsn++;
	logger->inbuf.max_lsn_in_buf = logger->lsn;
	ml_unlock(&logger->input_lock);
    }

    r = toku_logger_close(&logger);                                  assert(r == 0);
    {
        toku_struct_stat statbuf;
        sprintf(logname,
                "%s/log000000000000.tokulog%d",
                TOKU_TEST_FILENAME,
                TOKU_LOG_VERSION);
        r = toku_stat(logname, &statbuf, toku_uninstrumented);
        assert(r == 0);
        assert(statbuf.st_size == 12 + 5);
    }
    toku_os_recursive_delete(TOKU_TEST_FILENAME);
    return 0;
}
