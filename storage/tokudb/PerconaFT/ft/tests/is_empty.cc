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

#include "toku_os.h"
#include "cachetable/checkpoint.h"


#define FILENAME "test0.ft"

static void test_it (int N) {
    FT_HANDLE ft;
    int r;
    toku_os_recursive_delete(TOKU_TEST_FILENAME);
    r = toku_os_mkdir(TOKU_TEST_FILENAME, S_IRWXU);                                                                    CKERR(r);

    TOKULOGGER logger;
    r = toku_logger_create(&logger);                                                                        CKERR(r);
    r = toku_logger_open(TOKU_TEST_FILENAME, logger);                                                                  CKERR(r);


    CACHETABLE ct;
    toku_cachetable_create(&ct, 0, ZERO_LSN, logger);
    toku_cachetable_set_env_dir(ct, TOKU_TEST_FILENAME);

    toku_logger_set_cachetable(logger, ct);

    r = toku_logger_open_rollback(logger, ct, true);                                                        CKERR(r);

    TOKUTXN txn;
    r = toku_txn_begin_txn((DB_TXN*)NULL, (TOKUTXN)0, &txn, logger, TXN_SNAPSHOT_ROOT, false);                     CKERR(r);

    r = toku_open_ft_handle(FILENAME, 1, &ft, 1024, 256, TOKU_DEFAULT_COMPRESSION_METHOD, ct, txn, toku_builtin_compare_fun);                    CKERR(r);

    r = toku_txn_commit_txn(txn, false, NULL, NULL);                                 CKERR(r);
    toku_txn_close_txn(txn);
    CHECKPOINTER cp = toku_cachetable_get_checkpointer(ct);
    r = toku_checkpoint(cp, logger, NULL, NULL, NULL, NULL, CLIENT_CHECKPOINT);                             CKERR(r);
    r = toku_close_ft_handle_nolsn(ft, NULL);                                                                          CKERR(r);

    unsigned int rands[N];
    for (int i=0; i<N; i++) {
	r = toku_txn_begin_txn((DB_TXN*)NULL, (TOKUTXN)0, &txn, logger, TXN_SNAPSHOT_ROOT, false);                 CKERR(r);
	r = toku_open_ft_handle(FILENAME, 0, &ft, 1024, 256, TOKU_DEFAULT_COMPRESSION_METHOD, ct, txn, toku_builtin_compare_fun);                CKERR(r);
	r = toku_txn_commit_txn(txn, false, NULL, NULL);                             CKERR(r);
	toku_txn_close_txn(txn);

	r = toku_txn_begin_txn((DB_TXN*)NULL, (TOKUTXN)0, &txn, logger, TXN_SNAPSHOT_ROOT, false);                 CKERR(r);
	char key[100],val[300];
	DBT k, v;
	rands[i] = random();
	snprintf(key, sizeof(key), "key%x.%x", rands[i], i);
	memset(val, 'v', sizeof(val));
	val[sizeof(val)-1]=0;
	toku_ft_insert(ft, toku_fill_dbt(&k, key, 1+strlen(key)), toku_fill_dbt(&v, val, 1+strlen(val)), txn);
	r = toku_txn_commit_txn(txn, false, NULL, NULL);                                 CKERR(r);
	toku_txn_close_txn(txn);


	r = toku_checkpoint(cp, logger, NULL, NULL, NULL, NULL, CLIENT_CHECKPOINT);                             CKERR(r);
	r = toku_close_ft_handle_nolsn(ft, NULL);                                                                          CKERR(r);

	if (verbose) printf("i=%d\n", i);
    }
    for (int i=0; i<N; i++) {
	r = toku_txn_begin_txn((DB_TXN*)NULL, (TOKUTXN)0, &txn, logger, TXN_SNAPSHOT_ROOT, false);                     CKERR(r);
	r = toku_open_ft_handle(FILENAME, 0, &ft, 1024, 256, TOKU_DEFAULT_COMPRESSION_METHOD, ct, txn, toku_builtin_compare_fun);                CKERR(r);
	r = toku_txn_commit_txn(txn, false, NULL, NULL);                                 CKERR(r);
	toku_txn_close_txn(txn);

	r = toku_txn_begin_txn((DB_TXN*)NULL, (TOKUTXN)0, &txn, logger, TXN_SNAPSHOT_ROOT, false);                     CKERR(r);
	char key[100];
	DBT k;
	snprintf(key, sizeof(key), "key%x.%x", rands[i], i);
	toku_ft_delete(ft, toku_fill_dbt(&k, key, 1+strlen(key)), txn);

	if (0) {
	bool is_empty;
        is_empty = toku_ft_is_empty_fast(ft);
	assert(!is_empty);
	}
	
	r = toku_txn_commit_txn(txn, false, NULL, NULL);                                 CKERR(r);
	toku_txn_close_txn(txn);

	r = toku_checkpoint(cp, logger, NULL, NULL, NULL, NULL, CLIENT_CHECKPOINT);                             CKERR(r);
	r = toku_close_ft_handle_nolsn(ft, NULL);                                                                          CKERR(r);

	if (verbose) printf("d=%d\n", i);
    }
    r = toku_txn_begin_txn((DB_TXN*)NULL, (TOKUTXN)0, &txn, logger, TXN_SNAPSHOT_ROOT, false);                        CKERR(r);
    r = toku_open_ft_handle(FILENAME, 0, &ft, 1024, 256, TOKU_DEFAULT_COMPRESSION_METHOD, ct, txn, toku_builtin_compare_fun);                       CKERR(r);
    r = toku_txn_commit_txn(txn, false, NULL, NULL);                                     CKERR(r);
    toku_txn_close_txn(txn);

    if (0) {
    bool is_empty;
    is_empty = toku_ft_is_empty_fast(ft);
    assert(is_empty);
    }

    r = toku_checkpoint(cp, logger, NULL, NULL, NULL, NULL, CLIENT_CHECKPOINT);                                CKERR(r);
    r = toku_close_ft_handle_nolsn(ft, NULL);                                                                             CKERR(r);

    r = toku_checkpoint(cp, logger, NULL, NULL, NULL, NULL, CLIENT_CHECKPOINT);                                CKERR(r);
    toku_logger_close_rollback(logger);
    r = toku_checkpoint(cp, logger, NULL, NULL, NULL, NULL, CLIENT_CHECKPOINT);                                CKERR(r);
    toku_cachetable_close(&ct);
    r = toku_logger_close(&logger);                                                        assert(r==0);

}
    

int test_main (int argc, const char *argv[]) {
    default_parse_args(argc, argv);
    for (int i=1; i<=64; i++) {
	test_it(i);
    }
    return 0;
}
