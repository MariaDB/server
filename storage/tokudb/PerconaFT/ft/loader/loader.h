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

#pragma once

#include "toku_portability.h"
#include "ft/txn/txn.h"
#include "ft/cachetable/cachetable.h"
#include "ft/comparator.h"
#include "ft/ft-ops.h"

// The loader callbacks are C functions and need to be defined as such

typedef void (*ft_loader_error_func)(DB *, int which_db, int err, DBT *key, DBT *val, void *extra);

typedef int (*ft_loader_poll_func)(void *extra, float progress);

typedef struct ft_loader_s *FTLOADER;

int toku_ft_loader_open (FTLOADER *bl,
                          CACHETABLE cachetable,
			  generate_row_for_put_func g,
			  DB *src_db,
			  int N,
			  FT_HANDLE ft_hs[/*N*/], DB* dbs[/*N*/],
			  const char * new_fnames_in_env[/*N*/],
			  ft_compare_func bt_compare_functions[/*N*/],
			  const char *temp_file_template,
                          LSN load_lsn,
                          TOKUTXN txn,
                          bool reserve_memory,
                          uint64_t reserve_memory_size,
                          bool compress_intermediates,
                          bool allow_puts);

int toku_ft_loader_put (FTLOADER bl, DBT *key, DBT *val);

int toku_ft_loader_close (FTLOADER bl,
			   ft_loader_error_func error_callback, void *error_callback_extra,
			   ft_loader_poll_func  poll_callback,  void *poll_callback_extra);

int toku_ft_loader_abort(FTLOADER bl, 
                          bool is_error);

// For test purposes only
void toku_ft_loader_set_size_factor(uint32_t factor);

size_t ft_loader_leafentry_size(size_t key_size, size_t val_size, TXNID xid);
